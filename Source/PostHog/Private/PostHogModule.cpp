#include "Modules/ModuleManager.h"
#include "PostHogClient.h"
#include "PostHogSubsystem.h"
#include "PostHogReplayRecorder.h"
#include "posthog_unreal.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHog, Log, All);

namespace
{
// SDK diagnostics, otherwise lost: delivery, storage, startup and API misuse warnings.
void ForwardLog(void*, int32_t Level, phu_bytes Message)
{
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Message.data), static_cast<int32>(Message.len));
    const FString Line(Text.Length(), Text.Get());
    if (Level == PHU_LOG_ERROR) { UE_LOG(LogPostHog, Error, TEXT("%s"), *Line); }
    else { UE_LOG(LogPostHog, Warning, TEXT("%s"), *Line); }
}
}

class FPostHogModule final : public IModuleInterface
{
public:
    virtual bool SupportsDynamicReloading() override { return false; }
    virtual void ShutdownModule() override
    {
        FPostHogClient::ShutdownAll();
        UPostHogSubsystem::RetireInitializationOwners();
        FPostHogReplayRecorder::StopAll();
        phu_set_log(nullptr, nullptr);
    }
    virtual void StartupModule() override
    {
        if (phu_abi_version() != PHU_ABI_VERSION)
        {
            UE_LOG(LogPostHog, Error, TEXT("PostHog native ABI version mismatch."));
            return;
        }
        phu_set_log(&ForwardLog, nullptr);
    }
};

IMPLEMENT_MODULE(FPostHogModule, PostHog)
