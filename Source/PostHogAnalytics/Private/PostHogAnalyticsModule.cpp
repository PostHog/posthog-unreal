#include "Interfaces/IAnalyticsProviderModule.h"
#include "Modules/ModuleManager.h"
#include "PostHogAnalyticsProvider.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogAnalyticsModule, Log, All);

/** Resolves an explicitly bound subsystem; this module never creates a client. */
class FPostHogAnalyticsModule final : public IAnalyticsProviderModule
{
public:
    virtual bool SupportsDynamicReloading() override { return false; }
    virtual TSharedPtr<IAnalyticsProvider> CreateAnalyticsProvider(const FAnalyticsProviderConfigurationDelegate& GetConfigValue) const override
    {
        // Credentials belong to subsystem setup. Reject configuration that would
        // otherwise appear to route this already-bound client to a different project.
        if (GetConfigValue.IsBound() &&
            (!GetConfigValue.Execute(TEXT("ProjectToken"), false).IsEmpty() ||
             !GetConfigValue.Execute(TEXT("Host"), false).IsEmpty() ||
             !GetConfigValue.Execute(TEXT("ClientName"), false).IsEmpty()))
        {
            UE_LOG(LogPostHogAnalyticsModule, Warning, TEXT("Configure PostHog credentials and client options on the subsystem, then bind it. Analytics INI/delegate client configuration is not supported."));
            return nullptr;
        }
        return FPostHogAnalyticsProvider::GetBoundProvider();
    }
};

IMPLEMENT_MODULE(FPostHogAnalyticsModule, PostHogAnalytics)
