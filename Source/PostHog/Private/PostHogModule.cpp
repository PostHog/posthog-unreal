#include "Modules/ModuleManager.h"
#include "PostHogClient.h"
#include "posthog_unreal.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHog, Log, All);

class FPostHogModule final : public IModuleInterface
{
public:
    virtual bool SupportsDynamicReloading() override { return false; }
    virtual void ShutdownModule() override { FPostHogClient::ShutdownAll(); }
    virtual void StartupModule() override
    {
        if (phu_abi_version() != PHU_ABI_VERSION)
        {
            UE_LOG(LogPostHog, Error, TEXT("PostHog native ABI version mismatch."));
        }
    }
};

IMPLEMENT_MODULE(FPostHogModule, PostHog)
