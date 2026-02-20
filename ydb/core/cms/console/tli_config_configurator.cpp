#include "tli_config_configurator.h"
#include "configs_dispatcher.h"
#include "console.h"
#include <ydb/core/base/appdata_fwd.h>
#include <ydb/library/actors/core/actor_bootstrapped.h>

namespace NKikimr::NConsole {

    class TTliConfigConfigurator : public TActorBootstrapped<TTliConfigConfigurator> {
    public:
        static constexpr auto ActorActivityType() {
            return NKikimrServices::TActivity::TLI_CONFIG_CONFIGURATOR;
        }

        void Bootstrap() {
            Become(&TThis::StateWork);

            ui32 item = static_cast<ui32>(NKikimrConsole::TConfigItem::TliConfigItem);
            Send(MakeConfigsDispatcherID(SelfId().NodeId()),
                    new TEvConfigsDispatcher::TEvSetConfigSubscriptionRequest(item));
        }

        void Handle(TEvConsole::TEvConfigNotificationRequest::TPtr& ev) {
            const auto& record = ev->Get()->Record;
            LOG_INFO_S(*TlsActivationContext, NKikimrServices::CMS_CONFIGS,
                "TTliConfigConfigurator: new config: " << record.GetConfig().ShortDebugString());

            AppDataVerified().TliConfig.CopyFrom(record.GetConfig().GetTliConfig());

            Send(ev->Sender, new TEvConsole::TEvConfigNotificationResponse(record), 0, ev->Cookie);
        }

    private:
        STFUNC(StateWork) {
            switch (ev->GetTypeRewrite()) {
                hFunc(TEvConsole::TEvConfigNotificationRequest, Handle);
                IgnoreFunc(TEvConfigsDispatcher::TEvSetConfigSubscriptionResponse);
            }
        }
    };

    IActor* CreateTliConfigConfigurator() {
        return new TTliConfigConfigurator();
    }

} // namespace NKikimr::NConsole
