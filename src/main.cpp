// PipboyDoF - F4SE plugin (CommonLibF4, 旧版 1.10.163 対応)
//
// 普段は被写界深度(DoF)を無効にし、Pip-Boyメニューを開いている間だけ有効にする。
// DoFの設定は Fallout4Prefs.ini の bDoDepthOfField(セクション [Imagespace])。
//
// ログ: ドキュメント\My Games\Fallout4\F4SE\PipboyDoF.log (起動のたびに作り直される)

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

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
	// true にすると、すべてのメニューの開閉と、切り替えのたびのログを出す(不具合調査用)
	constexpr bool kVerbose = false;

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

			// Pip-Boyを開いたらDoF有効、閉じたら無効
			if (a_event.menuName == kPipboyMenu) {
				SetDoF(a_event.opening);
			}
			return RE::BSEventNotifyControl::kContinue;
		}
	};

	void OnF4SEMessage(F4SE::MessagingInterface::Message* a_msg)
	{
		if (a_msg->type == F4SE::MessagingInterface::kGameDataReady) {
			// 起動時は常にDoF無効(結果はログに残す)
			SetDoF(false, true);

			if (auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink(MenuSink::GetSingleton());
				Log("MenuSink registered");
			} else {
				Log("UI singleton not found");
			}
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
