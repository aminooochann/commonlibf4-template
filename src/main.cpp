// PipboyDoF - F4SE plugin (CommonLibF4, 旧版 1.10.163 対応)
//
// 普段は被写界深度(DoF)を無効にし、次の間だけ有効にする:
//   - Pip-Boyメニューを開いている間
//   - 水中にいる間(泳いでいる間と、潜っている間)
// DoFの設定は Fallout4Prefs.ini の bDoDepthOfField(セクション [Imagespace])。
//
// 負荷を抑える工夫:
//   - DoFの設定の検索は、起動時に1回だけ行い、以後は保存したポインタを使う
//   - 水中の監視は 0.1 秒ごとに「ビット1つとfloat1つ」を読むだけ。状態が変わったときだけ処理する
//   - ログは、起動時と失敗時だけ(メニューの開閉などは記録しない)
//
// ログ: ドキュメント\My Games\Fallout4\F4SE\PipboyDoF.log (起動のたびに作り直される)

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <atomic>
#include <chrono>
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
	// moveMode の中で「泳ぎ中」を表すビットの位置(ログで確認済み: 泳ぎ中は 0x0400 が立つ)
	constexpr std::uint32_t kSwimBit = 10;

	// 水中の監視間隔
	constexpr auto kPollInterval = std::chrono::milliseconds(100);

	// DoFの設定名の候補(INIのセクションによって名前が変わるため、順に試す)
	constexpr const char* kDoFSettingNames[] = {
		"bDoDepthOfField:Imagespace",
		"bDoDepthOfField:Display",
		"bDepthOfField:Imagespace",
		"bDepthOfField:Display",
	};
	constexpr auto kPipboyMenu = "PipboyMenu";

	RE::Setting* g_dofSetting = nullptr;  // 起動時に1回だけ取得する
	std::atomic<bool> g_pipboyOpen{ false };
	std::atomic<bool> g_underwater{ false };
	std::atomic<bool> g_dofEnabled{ false };  // 現在DoFを有効にしているか
	std::mutex g_applyLock;

	// ---- ログ ----

	std::mutex g_logLock;
	bool g_logStarted = false;

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

	// ---- DoFの切り替え ----

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

	// 「Pip-Boyを開いている」または「水中にいる」ときだけDoFを有効にする。
	// 状態が変わったときだけ設定を書き換える。
	void Apply()
	{
		if (!g_dofSetting) {
			return;
		}

		const auto desired = [] { return g_pipboyOpen.load() || g_underwater.load(); };

		// 変化がなければ、ロックも取らずにすぐ戻る(大半の呼び出しはここで終わる)
		if (desired() == g_dofEnabled.load()) {
			return;
		}

		std::lock_guard<std::mutex> lock(g_applyLock);
		const bool enable = desired();
		if (enable != g_dofEnabled.load()) {
			g_dofEnabled.store(enable);
			g_dofSetting->SetBinary(enable);
		}
	}

	// ---- 水中の監視 ----

	// プレイヤーが水中(泳ぎ中、または潜水中)か。
	//   泳ぎ中: moveMode の泳ぎビット  /  潜水中: underWaterTimer が動いている間
	bool IsUnderwater()
	{
		auto player = RE::PlayerCharacter::GetSingleton();
		if (!player) {
			return false;
		}

		const std::uint32_t moveMode = player->moveMode;
		return ((moveMode >> kSwimBit) & 1u) != 0 || player->underWaterTimer > 0.0f;
	}

	void StartWaterWatcher()
	{
		std::thread([] {
			bool last = false;

			while (true) {
				std::this_thread::sleep_for(kPollInterval);

				const bool now = IsUnderwater();
				if (now != last) {
					last = now;
					g_underwater.store(now);
					Apply();
				}
			}
		}).detach();
	}

	// ---- Pip-Boyメニューの監視 ----

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
			if (a_event.menuName == kPipboyMenu) {
				g_pipboyOpen.store(a_event.opening);
				Apply();
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ---- 起動処理 ----

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type != F4SE::MessagingInterface::kGameDataReady) {
			return;
		}

		std::string where;
		g_dofSetting = FindDoFSetting(where);
		if (!g_dofSetting) {
			Log("DoF setting NOT found: " + where);
			return;  // 設定が無ければ何もできないので、監視も始めない
		}

		// 起動時は常にDoF無効
		g_dofSetting->SetBinary(false);
		Log("DoF setting found in " + where);

		if (auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink(MenuSink::GetSingleton());
		} else {
			Log("UI singleton not found");
		}

		StartWaterWatcher();
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

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	} else {
		Log("messaging interface not found");
	}

	return true;
}
