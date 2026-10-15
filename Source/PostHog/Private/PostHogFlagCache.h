#pragma once

#include "PostHogFeatureFlags.h"

/** Native-only cache. All access is serialized by its owning client's mutex. */
class FPostHogFlagCache
{
public:
    bool bLoaded = false;
    TMap<FString, FPostHogFeatureFlagResult> Flags;
    TSet<FString> Reported;
    uint64 Revision = 0;
    /** Remote replay config; unset when the server disabled replay or never sent it. */
    TOptional<FPostHogProperties> SessionRecording;

    void Clear();
    EPostHogFlagReloadStatus Apply(const TSharedRef<FJsonObject>& Response);
    FPostHogProperties Serialize() const;
    bool Restore(const FPostHogProperties& Saved);
    void RestoreSessionRecording(const TSharedPtr<FJsonObject>& Saved);
    FPostHogProperties Values() const;
    static FPostHogProperties Exposure(const FString& Key, const FPostHogFeatureFlagResult& Flag);
    static bool IsMinimal(const FPostHogFeatureFlagResult& Flag);
    static void Minimize(FPostHogProperties& Properties);
};
