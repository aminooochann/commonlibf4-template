// PipboyDoF - F4SE plugin (CommonLibF4, 旧版 1.10.163 対応)
//
// 被写界深度(DoF)の動作:
//   - ゲーム起動時(メインメニューなど)は、DoFを有効にする
//   - セーブをロードして(またはニューゲームを始めて)ロード画面が終わったら、DoFを無効にする
//   - それ以降は、Pip-Boyメニューを開いている間だけDoFを有効にする(水中などでは有効にしない)
// DoFの設定は Fallout4Prefs.ini の bDoDepthOfField(セクション [Imagespace])。
//
// 負荷を抑える工夫:
//   - DoFの設定の検索は、起動時に1回だけ行い、以後は保存したポインタを使う
//   - 常時動く処理(監視スレッドなど)はなく、メニューの開閉と、ロード完了の通知のときだけ動く
//   - ログは、起動時と失敗時だけ
//
// ログ: ドキュメント\My Games\Fallout4\F4SE\PipboyDoF.log (起動のたびに作り直される)

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <atomic>
#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

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
	// DoFの設定名の候補(INIのセクションによって名前が変わるため、順に試す)
	constexpr const char* kDoFSettingNames[] = {
		"bDoDepthOfField:Imagespace",
		"bDoDepthOfField:Display",
		"bDepthOfField:Imagespace",
		"bDepthOfField:Display",
	};

	constexpr auto kPipboyMenu = "PipboyMenu";
	constexpr auto kLoadingMenu = "LoadingMenu";
	constexpr auto kFaderMenu = "FaderMenu";
	constexpr auto kHUDMenu = "HUDMenu";

	RE::Setting* g_dofSetting = nullptr;  // 起動時に1回だけ取得する

	std::atomic<bool> g_startupHold{ true };     // 起動後、最初のロード完了までDoFを有効にしておく
	std::atomic<bool> g_gameLoaded{ false };     // セーブのロード/ニューゲームが済んだか
	std::atomic<bool> g_loadingMenuOpen{ false };
	std::atomic<bool> g_pipboyOpen{ false };
	std::atomic<bool> g_dofEnabled{ false };     // 現在DoFを有効にしているか
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

	// 「起動後まだロードしていない」または「Pip-Boyを開いている」ときだけDoFを有効にする。
	// 状態が変わったときだけ設定を書き換える。
	void Apply()
	{
		if (!g_dofSetting) {
			return;
		}

		const auto desired = [] { return g_startupHold.load() || g_pipboyOpen.load(); };

		// 変化がなければ、ロックも取らずにすぐ戻る
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

	// ロードが済んでいて、ロード画面も消えていれば、起動時の「DoF有効」を解除する。
	void TryReleaseStartupHold()
	{
		if (g_startupHold.load() && g_gameLoaded.load() && !g_loadingMenuOpen.load()) {
			g_startupHold.store(false);
			Apply();
		}
	}

	// ---- メニューの監視 ----

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
			const auto& name = a_event.menuName;

			if (name == kPipboyMenu) {
				g_pipboyOpen.store(a_event.opening);
				Apply();
			} else if (name == kLoadingMenu) {
				g_loadingMenuOpen.store(a_event.opening);
				if (!a_event.opening) {
					TryReleaseStartupHold();
				}
			} else if (name == kHUDMenu) {
				// ゲームプレイ画面に入った = ロードが済んだ(通知を取りこぼしたときの保険)
				if (a_event.opening) {
					g_gameLoaded.store(true);
				}
			} else if (name == kFaderMenu) {
				// 画面の暗転(フェード)が終わった
				if (!a_event.opening) {
					TryReleaseStartupHold();
				}
			}

			return RE::BSEventNotifyControl::kContinue;
		}
	};

	// ---- 起動処理 ----

	void OnGameDataReady()
	{
		std::string where;
		g_dofSetting = FindDoFSetting(where);
		if (!g_dofSetting) {
			Log("DoF setting NOT found: " + where);
			return;  // 設定が無ければ何もできない
		}

		// 起動時はDoF有効(最初のロード完了まで)
		g_dofSetting->SetBinary(true);
		g_dofEnabled.store(true);
		Log("DoF setting found in " + where);

		if (auto ui = RE::UI::GetSingleton()) {
			ui->RegisterSink(MenuSink::GetSingleton());
		} else {
			Log("UI singleton not found");
		}
	}

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		switch (a_msg->type) {
		case F4SE::MessagingInterface::kGameDataReady:
			OnGameDataReady();
			break;

		case F4SE::MessagingInterface::kPostLoadGame:
		case F4SE::MessagingInterface::kNewGame:
			g_gameLoaded.store(true);
			TryReleaseStartupHold();
			break;

		default:
			break;
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

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	} else {
		Log("messaging interface not found");
	}

	return true;
}
