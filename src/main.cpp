// PipboyDoF - F4SE plugin (CommonLibF4)
// 普段は被写界深度(DoF)を無効、Pip-Boyメニューを開いている間だけ有効にする。
//
// 注意:
//  - 未ビルド・未テストです。CommonLibF4のバージョン(OG/NG)によって
//    プラグイン宣言部分やAPI名が少し違うので、コンパイルエラーが出たら調整が必要です。
//  - bDoDepthOfField を実行中に書き換えて即座に描画へ反映されるかは、実機で確認が必要です。

#include "F4SE/F4SE.h"
#include "RE/Fallout.h"

namespace
{
	constexpr auto kDoFSetting = "bDoDepthOfField:Display";
	constexpr auto kPipboyMenu = "PipboyMenu";

	void SetDoF(bool a_enable)
	{
		auto ini = RE::INISettingCollection::GetSingleton();
		if (!ini) {
			return;
		}

		auto setting = ini->GetSetting(kDoFSetting);
		if (!setting) {
			return;
		}

		setting->SetBinary(a_enable);
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
			// 起動時は常にDoF無効
			SetDoF(false);

			if (auto ui = RE::UI::GetSingleton()) {
				ui->RegisterSink(MenuSink::GetSingleton());
			}
		}
	}
}

// プラグインのバージョン情報は、libxse/commonlibf4-template の xmake.lua 側で
// 自動生成されるため、ここには書かない(二重定義になるため)。
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* a_f4se)
{
	F4SE::Init(a_f4se);

	if (auto messaging = F4SE::GetMessagingInterface()) {
		messaging->RegisterListener(OnF4SEMessage);
	}

	return true;
}
