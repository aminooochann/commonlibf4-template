// PipboyDoF - F4SE plugin (CommonLibF4, 旧版 1.10.163 対応)
//
// 普段は被写界深度(DoF)を無効にし、次の間だけ有効にする:
//   - Pip-Boyメニューを開いている間
//   - 水中にいる間(泳いでいる/潜っている間)
// DoFの設定は Fallout4Prefs.ini の bDoDepthOfField(セクション [Imagespace])。
//
// ログ: ドキュメント\My Games\Fallout4\F4SE\PipboyDoF.log (起動のたびに作り直される)

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#ifndef NOMINMAX
#	define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#	define WIN32_LEAN_AND_MEAN
#endif
#include <ShlObj.h>
#pragma comment(lib, "shell32.lib")

namespace
{
	// true にすると、すべてのメニューの開閉と、DoF切り替えのたびのログを出す(不具合調査用)
	constexpr bool kVerbose = false;

	// true にすると、水中判定に使う値(moveModeとunderWaterTimer)が変化するたびにログを出す。
	// 水中判定が正しく動くか確認するための一時的な診断用。確認できたら false にする。
	constexpr bool kLogWaterState = true;

	// 泳ぎ状態を表す moveMode のビット位置(Skyrimと同じ並びなら 10)。ログで確認する。
	constexpr std::uint32_t kSwimBit = 10;

	// DoFの設定名の候補。INIの [セクション] によって名前が変わるため、複数を順に試す。
	constexpr const char* kDoFSettingNames[] = {
		"bDoDepthOfField:Imagespace",
		"bDoDepthOfField:Display",
		"bDepthOfField:Imagespace",
		"bDepthOfField:Display",
	};
	constexpr auto kPipboyMenu = "PipboyMenu";

	std::mutex g_logLock;
	bool g_logStarted = false;

	std::mutex g_applyLock;
	std::atomic<bool> g_pipboyOpen{ false };
	std::atomic<bool> g_underwater{ false };
	bool g_dofApplied = false;  // 現在DoFを有効にしているか(g_applyLockで保護)

	// ドキュメント\My Games\<ゲーム名>\F4SE フォルダを探す。見つからなければ空のパスを返す。
	std::filesystem::path FindF4SELogDir()
	{
		wchar_t buffer[MAX_PATH]{};
		if (FAILED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buffer))) {
			return {};
		}

		const std::filesystem::path myGames = std::filesystem::path(buffer) / L"My Games";
		for (const wchar_t* game : { L"Fallout4", L"Fallout4 GOG", L"Fallout4 EPIC" }) {
			std::error_code ec;
			const auto dir = myGames / game / L"F4SE";
			if (std::filesystem::is_directory(dir, ec)) {
				return dir;
			}
		}
		return {};
	}

	void Log(const std::string& a_msg)
	{
		std::lock_guard<std::mutex> lock(g_logLock);

		try {
			static const std::filesystem::path path = [] {
				auto dir = FindF4SELogDir();
				return dir.empty() ? std::filesystem::temp_directory_path() / "PipboyDoF.log" : dir / "PipboyDoF.log";
			}();

			// 起動後の最初の書き込みだけ、ファイルを作り直す(ログが増え続けないように)
			std::ofstream file(path, g_logStarted ? std::ios::app : std::ios::trunc);
			g_logStarted = true;
			file << a_msg << "\n";
		} catch (...) {
		}
	}

	RE::Setting* FindDoFSetting(std::string& a_where)
	{
		auto pref = RE::INIPrefSettingCollection::GetSingleton();
		auto ini = RE::INISettingCollection::GetSingleton();

		for (auto name : kDoFSettingNames) {
			if (pref) {
				if (auto setting = pref->GetSetting(name)) {
					a_where = std::string("INIPrefSettingCollection / ") + name;
					return setting;
				}
			}

			if (ini) {
				if (auto setting = ini->GetSetting(name)) {
					a_where = std::string("INISettingCollection / ") + name;
					return setting;
				}
			}
		}

		a_where = std::string("none (pref singleton ") + (pref ? "ok" : "NULL") +
		          ", ini singleton " + (ini ? "ok" : "NULL") + ")";
		return nullptr;
	}

	void SetDoF(bool a_enable, bool a_forceLog = false)
	{
		std::string where;
		auto setting = FindDoFSetting(where);
		if (!setting) {
			Log(std::string("SetDoF(") + (a_enable ? "true" : "false") + "): setting NOT found: " + where);
			return;
		}

		const bool before = setting->GetBinary();
		setting->SetBinary(a_enable);
		const bool after = setting->GetBinary();

		if (kVerbose || a_forceLog) {
			Log(std::string("SetDoF(") + (a_enable ? "true" : "false") + "): found in " + where +
				", before=" + (before ? "1" : "0") + ", after=" + (after ? "1" : "0"));
		}
	}

	// 「Pip-Boyを開いている」または「水中にいる」ときだけDoFを有効にする。状態が変わったときだけ切り替える。
	void Apply()
	{
		std::lock_guard<std::mutex> lock(g_applyLock);

		const bool desired = g_pipboyOpen.load() || g_underwater.load();
		if (desired != g_dofApplied) {
			g_dofApplied = desired;
			SetDoF(desired, kVerbose);
		}
	}

	// プレイヤーが水中(泳ぎ中・潜水中)かどうかを調べる。
	bool QueryUnderwater(std::uint32_t& a_moveMode, float& a_timer)
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		a_moveMode = player->moveMode;
		a_timer = player->underWaterTimer;

		const bool swimming = ((a_moveMode >> kSwimBit) & 1u) != 0;
		return swimming || a_timer > 0.0f;
	}

	void StartWaterWatcher()
	{
		std::thread([] {
			std::uint32_t lastMoveMode = 0xFFFFFFFFu;
			bool lastTimerPositive = false;
			bool lastUnderwater = false;

			while (true) {
				std::this_thread::sleep_for(std::chrono::milliseconds(100));

				try {
					std::uint32_t moveMode = 0;
					float timer = 0.0f;
					const bool underwater = QueryUnderwater(moveMode, timer);

					if (kLogWaterState) {
						const bool timerPositive = timer > 0.0f;
						if (moveMode != lastMoveMode || timerPositive != lastTimerPositive || underwater != lastUnderwater) {
							char buffer[160]{};
							std::snprintf(buffer, sizeof(buffer),
								"water: moveMode=0x%04X swimBit=%u underWaterTimer=%.2f -> underwater=%d",
								moveMode, (moveMode >> kSwimBit) & 1u, timer, underwater ? 1 : 0);
							Log(buffer);
							lastMoveMode = moveMode;
							lastTimerPositive = timerPositive;
							lastUnderwater = underwater;
						}
					}

					g_underwater.store(underwater);
					Apply();
				} catch (...) {
				}
			}
		}).detach();
	}

	class MenuSink : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
	{
	public:
		static MenuSink* GetSingleton()
		{
			static MenuSink singleton;
			return std::addressof(singleton);
		}

		RE::BSEventNotifyControl ProcessEvent(
			const RE::MenuOpenCloseEvent& a_event,
			RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
		{
			if (kVerbose) {
				Log(std::string("menu: ") + a_event.menuName.c_str() + (a_event.opening ? " opened" : " closed"));
			}

			// Pip-Boyを開いたらDoF有効、閉じたら(水中でなければ)無効
			if (a_event.menuName == kPipboyMenu) {
				g_pipboyOpen.store(a_event.opening);
				Apply();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			// 起動時は常にDoF無効(結果はログに残す)
			{
				std::lock_guard<std::mutex> lock(g_applyLock);
				g_dofApplied = false;
				SetDoF(false, true);
			}

			if (auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink(MenuSink::GetSingleton());
				Log("MenuSink registered");
			} else {
				Log("UI singleton not found");
			}

			StartWaterWatcher();
			Log("water watcher started");
		}
	}
}

// ---- F4SEにプラグインとして認識させるための宣言 ----
// 旧版ゲーム(1.10.163)用のF4SE 0.6.23 は、古い形式の F4SEPlugin_Query という関数で
// プラグインかどうかを判断する。
// ライブラリ側が F4SEPlugin_Query を既に提供している場合に二重定義にならないよう、
// 別名の関数として定義し、リンカーの /alternatename で「無い場合だけ」この関数を使う。
extern "C" bool PipboyDoF_QueryImpl(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info)
{
	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = "PipboyDoF";
	a_info->version = 1;
	return true;
}

#pragma comment(linker, "/alternatename:F4SEPlugin_Query=PipboyDoF_QueryImpl")
#pragma comment(linker, "/export:F4SEPlugin_Query")

// 新しいF4SE用の F4SEPlugin_Version は、テンプレート側(commonlibf4-plugin.cpp)が
// 自動生成するため、ここには書かない(書くと「already defined」で二重定義エラーになる)。

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	try {
		F4SE::Init(a_f4se);
	} catch (const std::exception& e) {
		Log(std::string("F4SE::Init threw: ") + e.what());
		return false;
	} catch (...) {
		Log("F4SE::Init threw an unknown exception");
		return false;
	}

	Log("PipboyDoF loaded");

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	} else {
		Log("messaging interface not found");
	}

	return true;
}
