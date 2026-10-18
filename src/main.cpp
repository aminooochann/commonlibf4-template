// PipboyDoF v2 - F4SE plugin (CommonLibF4)
// 普段は被写界深度(DoF)を無効、Pip-Boyメニューを開いている間だけ有効にする。
//
// v2の変更点:
//  - bDoDepthOfField は Fallout4Prefs.ini の設定なので、INIPrefSettingCollection側も探すようにした
//    (v1は INISettingCollection だけを見ていたため、設定が見つからず何も起きていなかった可能性が高い)
//  - 診断用ログ(PipboyDoF.log)を出力するようにした

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <exception>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace
{
	constexpr auto kDoFSetting = "bDoDepthOfField:Display";
	constexpr auto kPipboyMenu = "PipboyMenu";

	std::mutex g_logLock;

	// 診断用ログ。次の2か所に書く:
	//  (1) %TEMP% フォルダの PipboyDoF.log (エクスプローラーのアドレス欄に %TEMP% と入力すると開ける)
	//  (2) ゲームの実行フォルダ(MO2経由ならOverwriteフォルダ)の PipboyDoF.log
	void Log(const std::string& a_msg)
	{
		std::lock_guard<std::mutex> lock(g_logLock);

		try {
			std::ofstream file(std::filesystem::temp_directory_path() / "PipboyDoF.log", std::ios::app);
			file << a_msg << "\n";
		} catch (...) {
		}

		try {
			std::ofstream file("PipboyDoF.log", std::ios::app);
			file << a_msg << "\n";
		} catch (...) {
		}
	}

	RE::Setting* FindDoFSetting(const char*& a_where)
	{
		if (auto pref = RE::INIPrefSettingCollection::GetSingleton()) {
			if (auto setting = pref->GetSetting(kDoFSetting)) {
				a_where = "INIPrefSettingCollection";
				return setting;
			}
		}

		if (auto ini = RE::INISettingCollection::GetSingleton()) {
			if (auto setting = ini->GetSetting(kDoFSetting)) {
				a_where = "INISettingCollection";
				return setting;
			}
		}

		a_where = "none";
		return nullptr;
	}

	void SetDoF(bool a_enable)
	{
		const char* where = "none";
		auto setting = FindDoFSetting(where);
		if (!setting) {
			Log(std::string("SetDoF(") + (a_enable ? "true" : "false") + "): setting NOT found");
			return;
		}

		const bool before = setting->GetBinary();
		setting->SetBinary(a_enable);
		const bool after = setting->GetBinary();

		Log(std::string("SetDoF(") + (a_enable ? "true" : "false") + "): found in " + where +
			", before=" + (before ? "1" : "0") + ", after=" + (after ? "1" : "0"));
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
			// 診断用: すべてのメニューの開閉をログに残す(Pip-Boyのメニュー名の確認用)
			Log(std::string("menu: ") + a_event.menuName.c_str() + (a_event.opening ? " opened" : " closed"));

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
			Log("kGameDataReady");

			// 起動時は常にDoF無効
			SetDoF(false);

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
// プラグインかどうかを判断する。これが無いと「does not appear to be an F4SE plugin」になる。
//
// ライブラリ側が F4SEPlugin_Query を既に提供している場合に、二重定義エラーにならないよう、
// 「別名の関数」として定義し、リンカーの /alternatename で「無い場合だけ」この関数を使う。
extern "C" bool PipboyDoF_QueryImpl(const F4SE::QueryInterface*, F4SE::PluginInfo* a_info)
{
	Log("F4SEPlugin_Query called");
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
	Log("F4SEPlugin_Load entered (before Init)");

	try {
		F4SE::Init(a_f4se);
		Log("F4SE::Init ok");
	} catch (const std::exception& e) {
		Log(std::string("F4SE::Init threw: ") + e.what());
		return false;
	} catch (...) {
		Log("F4SE::Init threw an unknown exception");
		return false;
	}

	Log("PipboyDoF v3 loaded");

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
		Log("messaging listener registered");
	} else {
		Log("messaging interface not found");
	}

	return true;
}
