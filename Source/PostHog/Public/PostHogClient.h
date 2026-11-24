#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "PostHogFeatureFlags.h"
#include "PostHogProperties.h"
#include "PostHogTypes.h"

class FPostHogReplayFrame;

struct POSTHOG_API FPostHogConfig
{
    FString ProjectToken;
    FString Host = TEXT("https://us.i.posthog.com");
    /** Writable storage root. Empty selects memory-only storage. */
    FString StorageRoot;
    FString ClientName = TEXT("default");
    EPostHogPersonProfiles PersonProfiles = EPostHogPersonProfiles::IdentifiedOnly;
    EPostHogSessionMode SessionMode = EPostHogSessionMode::Automatic;
    bool bPersistSessions = false;
    bool bInitiallyOptedOut = false;
    /** Subsystem-level lifecycle capture; direct clients do not subscribe to engine delegates. */
    bool bCaptureApplicationLifecycleEvents = true;
    bool bPreloadFeatureFlags = true;
    bool bSendFeatureFlagEvents = true;
    int32 FlushAt = 20;
    int32 FlushIntervalMilliseconds = 30000;
    int32 MaxQueueSize = 1000;
    int32 MaxBatchSize = 50;
    int32 RequestTimeoutSeconds = 10;
    int32 ShutdownTimeoutMilliseconds = 3000;
    FPostHogReplaySettings Replay;
    /** Engine/platform context properties sampled before client creation. */
    FPostHogProperties Context;
    /**
     * Synchronous event filter/mutator. Return false to drop. Must be thread-safe and non-throwing.
     * May still finish or be destroyed asynchronously off the game thread after ShutdownAsync() / ~FPostHogClient(),
     * so captures must be thread-safe and self-owning (values, TSharedPtr/TWeakPtr/TWeakObjectPtr — not raw stack pointers or references).
     */
    TFunction<bool(FJsonObject&)> BeforeSend;
    /**
     * Optional feature-flags update listener invoked on an SDK background thread. Must be thread-safe and non-throwing.
     * May still finish or be destroyed asynchronously off the game thread after ShutdownAsync() / ~FPostHogClient(),
     * so captures must be thread-safe and self-owning (values, TSharedPtr/TWeakPtr/TWeakObjectPtr — not raw stack pointers or references).
     */
    TFunction<void(FPostHogFeatureFlagsUpdate)> OnFeatureFlagsChanged;
};

/** Thin thread-safe Unreal adapter over the stateful Rust SDK (`posthog-stateful-ffi`). */
class POSTHOG_API FPostHogClient final
{
public:
    using FPtr = TSharedPtr<FPostHogClient, ESPMode::ThreadSafe>;

    static TSharedFuture<FPtr> CreateAsync(FPostHogConfig Config);
    ~FPostHogClient();

    bool IsInitialized() const;
    bool IsDisabled() const;
    bool IsOptedOut() const;

    EPostHogCaptureResult Capture(const FString& Event, const FPostHogProperties& Properties = {}, const FPostHogProperties& Options = {});
    EPostHogCaptureResult Screen(const FString& Name, const FPostHogProperties& Properties = {});

    void Identify(const FString& Id, const FPostHogProperties& Set = {}, const FPostHogProperties& SetOnce = {});
    void Reset();
    void Alias(const FString& AliasId);
    FString GetDistinctId() const;
    FString GetAnonymousId() const;

    void Register(const FPostHogProperties& Properties);
    void Unregister(const FString& Key);
    void SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce = {});
    void Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties = {});
    void GroupIdentify(const FString& Type, const FString& Key, const FPostHogProperties& Properties = {});
    void RemoveGroup(const FString& Type);

    void OptOut();
    void OptIn();

    TSharedFuture<FPostHogDrainResult> FlushAsync();
    TSharedFuture<FPostHogDrainResult> ShutdownAsync();

    FPostHogFeatureFlagResult GetFeatureFlagResult(const FString& Key, EPostHogFlagReporting Reporting = EPostHogFlagReporting::Default);
    bool IsFeatureEnabled(const FString& Key, EPostHogFlagReporting Reporting = EPostHogFlagReporting::Default, bool bDefaultValue = false);
    FPostHogFeatureFlags GetFeatureFlags() const;
    void ReloadFeatureFlags();
    void SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload = true);
    void SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload = true);
    void ResetPersonPropertiesForFlags(bool bReload = true);
    void ResetGroupPropertiesForFlags(const FString& Type = FString(), bool bReload = true);

    FString GetSessionId();
    FString PeekSessionId() const;
    void StartSession();
    void EndSession();
    void ResetSession();
    void SessionActivity();
    void SetAppInBackground(bool bBackground);

    void StartSessionReplay();
    void StopSessionReplay();
    bool IsSessionReplayActive() const;

private:
    friend class FPostHogModule;
    friend class FPostHogAnalyticsProvider;
    friend class FPostHogReplayFrame;
    friend class FPostHogReplayRecorder;
    friend class UPostHogSubsystem;

    static FPtr CreateSync(FPostHogConfig Config);
    FPostHogDrainResult ShutdownSync();
    static bool LaunchTrackedTask(TFunction<void()>&& Task);

    EPostHogCaptureResult CaptureWithFallback(const FString& Event, const FPostHogProperties& Properties,
        const FPostHogProperties& Fallback, const FPostHogProperties& Options = {});

    enum class EReplayFrameStatus : uint8 { Ok, Throttled, Inactive };
    EReplayFrameStatus BeginReplayFrame(int32 Width, int32 Height, int32 RowStrideBytes, TUniquePtr<FPostHogReplayFrame>& OutFrame);

    static void ShutdownAll();

    struct FImpl;
    TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
    explicit FPostHogClient(TSharedPtr<FImpl, ESPMode::ThreadSafe> InImpl) : Impl(MoveTemp(InImpl)) {}
    static TArray<TWeakPtr<FImpl, ESPMode::ThreadSafe>> LiveClients;
};
