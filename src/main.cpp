// PipboyDoF v2 - F4SE plugin (CommonLibF4)
// 普段は被写界深度(DoF)を無効、Pip-Boyメニューを開いている間だけ有効にする。
//
// v2の変更点:
//  - bDoDepthOfField は Fallout4Prefs.ini の設定なので、INIPrefSettingCollection側も探すようにした
//    (v1は INISettingCollection だけを見ていたため、設定が見つからず何も起きていなかった可能性が高い)
//  - 診断用ログ(PipboyDoF.log)を出力するようにした

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

#include <fstream>
#include <mutex>
#include <string>

namespace
{
	constexpr auto kDoFSetting = "bDoDepthOfField:Display";
	constexpr auto kPipboyMenu = "PipboyMenu";

	std::mutex g_logLock;

	// ゲームの実行フォルダ(MO2経由ならOverwriteフォルダ)に PipboyDoF.log を書く
	void Log(const std::string& a_msg)
	{
		std::lock_guard<std::mutex> lock(g_logLock);
		std::ofstream file("PipboyDoF.log", std::ios::app);
		file << a_msg << "\n";
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
// f4se.log に「does not appear to be an F4SE plugin」と出た原因は、この宣言が無かったこと。
// 対象のゲームは 1.10.163(旧版)。
F4SE_EXPORT constinit auto F4SEPlugin_Version = []() noexcept {
	F4SE::PluginVersionData data{};
	data.PluginVersion({ 1, 0, 0, 0 });
	data.PluginName("PipboyDoF");
	data.AuthorName("user");
	data.UsesAddressLibrary(true);
	data.IsLayoutDependent(true);
	data.HasNoStructUse(false);
	data.CompatibleVersions({ REL::Version{ 1, 10, 163, 0 } });
	return data;
}();

F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);
	Log("PipboyDoF v2 loaded");

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	}

	return true;
}
