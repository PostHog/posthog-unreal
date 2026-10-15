#include "PostHogSettings.h"

UPostHogSettings::UPostHogSettings()
{
    CategoryName = TEXT("Plugins");
    SectionName = TEXT("PostHog");
}

FPostHogConfig FPostHogOptions::ToNative() const
{
    FPostHogConfig Config;
    Config.ProjectToken = ProjectToken; Config.Host = Host; Config.ClientName = ClientName;
    Config.PersonProfiles = PersonProfiles; Config.SessionMode = SessionMode;
    Config.bInitiallyOptedOut = bInitiallyOptedOut;
    Config.bCaptureApplicationLifecycleEvents = bCaptureApplicationLifecycleEvents;
    Config.bPreloadFeatureFlags = bPreloadFeatureFlags; Config.bSendFeatureFlagEvents = bSendFeatureFlagEvents;
    Config.FlushAt = FlushAt; Config.FlushIntervalMilliseconds = FlushIntervalMilliseconds;
    Config.MaxQueueSize = MaxQueueSize; Config.MaxBatchSize = MaxBatchSize;
    Config.RequestTimeoutSeconds = RequestTimeoutSeconds; Config.ShutdownTimeoutMilliseconds = ShutdownTimeoutMilliseconds;
    if (!AppVersion.IsEmpty()) { Config.Context.SetString(TEXT("$app_version"), AppVersion); }
    if (!AppBuild.IsEmpty()) { Config.Context.SetString(TEXT("$app_build"), AppBuild); }
    return Config;
}
