#pragma once
#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Dom/JsonObject.h"
#include "PostHogProperties.h"
#include "PostHogFeatureFlags.h"
#include "PostHogTypes.h"

class FPostHogReplayFrame;

struct POSTHOG_API FPostHogConfig
{
    FString ProjectToken;
    FString Host = TEXT("https://us.i.posthog.com");
    /** Writable fresh state root. Empty selects memory for direct native clients. */
    FString StorageRoot;
    FString ClientName = TEXT("default");
    EPostHogPersonProfiles PersonProfiles = EPostHogPersonProfiles::IdentifiedOnly;
    EPostHogSessionMode SessionMode = EPostHogSessionMode::Automatic;
    bool bPersistSessions = false;
    bool bInitiallyOptedOut = false;
    /** Engine-owner lifecycle capture; direct clients do not observe application signals. */
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
    /** Sample engine/platform context before creation, on the game thread. */
    FPostHogProperties Context;
    /** Complete assembled SDK JSON: event, distinct_id, properties, options, groups,
     * timestamp (UTC ISO datetime without suffix) and uuid. Preserve unmodified fields.
     * Analytics runs inline on the capturing thread; replay runs on the Rust encoder.
     * Return false to drop. Must be thread-safe/non-throwing, with no off-thread UObject
     * access. Same-client recursive capture is rejected. Never wait for another capture.
     * Capture back-references weakly. Callback-origin flush/close report Deferred. */
    TFunction<bool(FJsonObject&)> BeforeSend;
};

/** Thin engine adapter. Local operations invoke the Rust SDK, which may no-op.
 * Ok does not promise a state change, persistence, analytics admission or ingestion.
 * Async completions are plain data; UObject consumers dispatch on the game thread.
 * Release client/frame references and join caller threads before module unload. */
class POSTHOG_API FPostHogClient final
{
public:
    using FPtr = TSharedPtr<FPostHogClient, ESPMode::ThreadSafe>;
    /** Pure opaque host/token/instance equality key using SDK normalization; root is separate. */
    static EPostHogResult CoordinationKey(const FPostHogConfig& Config, FString& OutKey);
    static TSharedFuture<FPtr> CreateAsync(FPostHogConfig Config);
    ~FPostHogClient();
    /** Native initialization completed, even if the SDK is disabled. */
    bool IsInitialized() const;
    bool IsDisabled() const;
    EPostHogResult Capture(const FString& Event, const FPostHogProperties& Properties = {}, const FPostHogProperties& Options = {});
    EPostHogResult Screen(const FString& Name, const FPostHogProperties& Properties = {});
    EPostHogResult Identify(const FString& Id, const FPostHogProperties& Set = {}, const FPostHogProperties& SetOnce = {});
    EPostHogResult Reset();
    EPostHogResult Alias(const FString& AliasId);
    FString GetDistinctId() const;
    FString GetAnonymousId() const;
    EPostHogResult Register(const FPostHogProperties& Properties);
    EPostHogResult Unregister(const FString& Key);
    EPostHogResult SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce = {});
    EPostHogResult Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties = {});
    EPostHogResult GroupIdentify(const FString& Type, const FString& Key, const FPostHogProperties& Properties = {});
    EPostHogResult RemoveGroup(const FString& Type);
    bool IsOptedOut() const;
    EPostHogResult OptOut();
    EPostHogResult OptIn();
    TSharedFuture<FPostHogDrainResult> FlushAsync();
    /** One shutdown operation: stops admission immediately and joins native/attached host work.
     * Native hooks and attached render-thread callers report Deferred; never block that thread on retirement. */
    TSharedFuture<FPostHogDrainResult> ShutdownAsync();

    FPostHogFeatureFlagResult GetFeatureFlagResult(const FString& Key, EPostHogFlagReporting Reporting = EPostHogFlagReporting::Default);
    FPostHogFeatureFlags GetFeatureFlags() const;
    EPostHogResult ReloadFeatureFlags();
    /** Takes one coalesced notification; Inactive means empty. No implicit dispatch. */
    EPostHogResult PollFeatureFlags(FPostHogFeatureFlagsUpdate& Update);
    EPostHogResult SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload = true);
    EPostHogResult SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload = true);
    EPostHogResult ResetPersonPropertiesForFlags(bool bReload = true);
    EPostHogResult ResetGroupPropertiesForFlags(const FString& Type = FString(), bool bReload = true);

    FString GetSessionId();
    /** Passive SDK peek; may return an expired session. */
    FString PeekSessionId() const;
    EPostHogResult StartSession();
    EPostHogResult EndSession();
    EPostHogResult ResetSession();
    EPostHogResult SessionActivity();
    EPostHogResult SetAppInBackground(bool bBackground);

    /** Manual recording request; remote permission and consent still apply. */
    EPostHogResult StartSessionReplay();
    EPostHogResult StopSessionReplay();
    bool IsSessionReplayActive() const;
    /** Reserve before acquisition. Host provides already composited/downscaled
     * top-down BGRA8888; any redaction must already have been applied by the producer.
     * The returned lease must be submitted once or abandoned; destruction abandons it. */
    EPostHogResult BeginReplayFrame(int32 Width, int32 Height, int32 RowStrideBytes, TUniquePtr<FPostHogReplayFrame>& OutFrame);

private:
    friend class FPostHogModule;
    friend class FPostHogAnalyticsProvider;
    EPostHogResult CaptureWithFallback(const FString& Event, const FPostHogProperties& Properties,
        const FPostHogProperties& Fallback, const FPostHogProperties& Options = {});
    friend class FPostHogReplayFrame;
    friend class FPostHogReplayRecorder;
#if WITH_DEV_AUTOMATION_TESTS
    friend struct FPostHogHostCloseTestAccess;
#endif
    using FHostClose = TPair<TSharedFuture<bool>, bool>;
    bool BindHostClose(TFunction<void()>&& Cancel, TFunction<FHostClose()>&& Retire);
    static void ShutdownAll();
    struct FImpl;
    TSharedPtr<FImpl, ESPMode::ThreadSafe> Impl;
    explicit FPostHogClient(TSharedPtr<FImpl, ESPMode::ThreadSafe> InImpl) : Impl(MoveTemp(InImpl)) {}
    static FCriticalSection ClientsMutex;
    static TArray<TSharedPtr<FImpl, ESPMode::ThreadSafe>> Clients;
    static bool bStopping;
};

/** Single-use native reservation. Owns no renderer resources and exposes no Rust buffer. */
class POSTHOG_API FPostHogReplayFrame final
{
public:
    ~FPostHogReplayFrame();
    FPostHogReplayFrame(const FPostHogReplayFrame&) = delete;
    FPostHogReplayFrame& operator=(const FPostHogReplayFrame&) = delete;
    int64 GetRequiredBytes() const { return RequiredBytes; }
    EPostHogResult Submit(const uint8* Pixels, int64 NumBytes);
    EPostHogResult Abandon();
private:
    friend class FPostHogClient;
    FPostHogReplayFrame(uint64 InToken, int64 InRequiredBytes, TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe> InOwner)
        : Token(InToken), RequiredBytes(InRequiredBytes), Owner(MoveTemp(InOwner)) {}
    uint64 Token;
    int64 RequiredBytes;
    TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe> Owner;
};
