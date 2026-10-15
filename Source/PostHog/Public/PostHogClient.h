#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "PostHogProperties.h"
#include "PostHogFeatureFlags.h"

#include "PostHogTypes.h"

struct POSTHOG_API FPostHogEvent
{
    FString Event;
    FString DistinctId;
    FString Uuid;
    FDateTime Timestamp;
    FPostHogProperties Properties;
private:
    friend class FPostHogClient;
    uint64 ConsentGeneration = 0;
};

struct POSTHOG_API FPostHogConfig
{
    FString ProjectToken;
    FString Host = TEXT("https://us.i.posthog.com");
    /** Writable saved-data root, never a content/plugin installation directory. */
    FString StorageRoot;
    /** Separate PIE instances and runtime environments must use distinct names. */
    FString ClientName = TEXT("default");
    EPostHogPersonProfiles PersonProfiles = EPostHogPersonProfiles::IdentifiedOnly;
    EPostHogSessionMode SessionMode = EPostHogSessionMode::Automatic;
    bool bInitiallyOptedOut = false;
    /** The lifecycle owner calls StartApplicationLifecycle once setup is ready. */
    bool bCaptureApplicationLifecycleEvents = true;
    bool bPreloadFeatureFlags = true;
    bool bSendFeatureFlagEvents = true;
    int32 FlushAt = 20;
    int32 FlushIntervalMilliseconds = 30000;
    int32 MaxQueueSize = 1000;
    int32 MaxBatchSize = 50;
    int32 RequestTimeoutSeconds = 10;
    int32 ShutdownTimeoutMilliseconds = 3000;
    /** Plain context sampled by the host on the game thread, not by capture. */
    FPostHogProperties Context;
    /** Runs on the capturing thread outside state locks. Reentrant calls are
     * allowed; consent is checked again afterwards. Return false to drop.
     * Hooks must be thread-safe, non-throwing, and must not access UObjects from
     * non-game threads. Recursive capture is the hook author's responsibility. */
    TFunction<bool(FPostHogEvent&)> BeforeSend;
    /** Injectable wall clock. Must be thread-safe and non-reentrant. */
    TFunction<FDateTime()> Now = [] { return FDateTime::UtcNow(); };
};

struct POSTHOG_API FPostHogDeliveryResult
{
    bool bCompleted = false;
    int64 Pending = 0;
    int64 Delivered = 0;
    int64 TerminallyDropped = 0;
    int64 StorageErrors = 0;
};

/** Instance-based native client. Capture and local state transitions are
 * thread-safe and perform no network or disk I/O on the calling thread.
 * Completion can run off-thread; UObject consumers must marshal to the game thread.
 */
class POSTHOG_API FPostHogClient final
{
public:
    using FPtr = TSharedPtr<FPostHogClient, ESPMode::ThreadSafe>;
    static TSharedFuture<FPtr> CreateAsync(FPostHogConfig Config);
    ~FPostHogClient();

    bool IsInitialized() const;
    EPostHogCaptureResult Capture(const FString& Event, const FPostHogProperties& Properties = {});
    EPostHogCaptureResult Screen(const FString& ScreenName, const FPostHogProperties& Properties = {});
    bool Identify(const FString& DistinctId, const FPostHogProperties& Set = {}, const FPostHogProperties& SetOnce = {});
    TSharedFuture<bool> ResetAsync();
    void Alias(const FString& AliasId);
    FString GetDistinctId() const;
    FString GetAnonymousId() const;
    void Register(const FPostHogProperties& Properties);
    void Unregister(const FString& Key);
    void SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce = {});
    void Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties = {});

    bool IsOptedOut() const;
    /** Stops admission before returning. Completion waits for state write and purge. */
    TSharedFuture<bool> OptOutAsync();
    TSharedFuture<bool> OptInAsync();
    TSharedFuture<FPostHogDeliveryResult> FlushAsync();
    TSharedFuture<bool> ShutdownAsync();

    /** Local reads only. Missing values/payloads return JSON null; the result
     * API additionally distinguishes missing flags and absent payloads. */
    FPostHogFeatureFlagResult GetFeatureFlagResult(const FString& Key, bool bSendEvent = true);
    FPostHogValue GetFeatureFlag(const FString& Key, bool bSendEvent = true);
    /** The caller default applies only when no value is available. */
    bool IsFeatureEnabled(const FString& Key, bool bSendEvent = true, bool bDefaultValue = false);
    FPostHogValue GetFeatureFlagPayload(const FString& Key);
    FPostHogProperties GetFeatureFlags() const;
    bool HasLoadedFeatureFlags() const;
    /** Concurrent reloads share one cycle. Context changes queue the latest
     * context; the shared future resolves after that context's load finishes. */
    TSharedFuture<FPostHogFlagReloadResult> ReloadFeatureFlagsAsync();
    bool SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload = true);
    bool SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload = true);
    void ResetPersonPropertiesForFlags(bool bReload = true);
    /** Empty Type clears all group-property overrides. */
    void ResetGroupPropertiesForFlags(const FString& Type = FString(), bool bReload = true);

    FString GetSessionId();
    bool StartSession();
    void EndSession();
    bool SetSessionId(const FString& SessionId);
    /** Uses $app_version/$app_build sampled into Context by the host. Markers
     * survive identity reset. Calling again on this client is a no-op. */
    void StartApplicationLifecycle(bool bInForeground = true);
    void OnBackground();
    void OnForeground();

private:
    friend class FPostHogModule;
    friend class UPostHogSubsystem;
    /** Recheck a completion before publishing it on the game thread. */
    FPostHogFlagReloadResult ValidateFlagReloadResult(FPostHogFlagReloadResult Result) const;
    /** Runtime module teardown: closes clients and joins all native work. */
    static void ShutdownAll();
    struct FImpl;
    TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
    explicit FPostHogClient(TSharedPtr<FImpl, ESPMode::ThreadSafe> InImpl) : Impl(MoveTemp(InImpl)) {}
    static FCriticalSection ClientsMutex;
    static TArray<TWeakPtr<FImpl, ESPMode::ThreadSafe>> Clients;
    static bool bStopping;
};
