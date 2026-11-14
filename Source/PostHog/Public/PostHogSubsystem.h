#pragma once
#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PostHogSettings.h"
#include "PostHogReplayFrame.h"
#include "PostHogSubsystem.generated.h"

UENUM(BlueprintType)
enum class EPostHogSubsystemState : uint8 { Idle, Initializing, Ready, ShuttingDown, Failed, Deinitialized };
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogSetupCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogShutdownCompleted, const FPostHogDrainResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogFeatureFlagsChanged, const FPostHogFeatureFlagsUpdate&, Update);
struct FPostHogInitialization;
struct FPostHogLifecycleSignals;
class FPostHogReplayRecorder;
class FPostHogAnalyticsProvider;
class IAnalyticsProvider;

/** One native client per game instance. All UObject entry points run on the game thread. */
UCLASS()
class POSTHOG_API UPostHogSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;
    /** Starts asynchronous local initialization, not a network readiness check.
     * OnSetupCompleted means a native instance exists; inspect IsDisabled separately. */
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    bool Setup(const FPostHogOptions& Options);
    /** Uses Saved/PostHogStateful by default and adds runtime/editor/PIE namespace inputs. */
    bool SetupWithConfig(FPostHogConfig Config, bool bEnableInEditor = false, bool bEnableOnDedicatedServer = false);
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    void Shutdown();
    UFUNCTION(BlueprintPure, Category = "PostHog")
    bool IsInitialized() const;
    UFUNCTION(BlueprintPure, Category = "PostHog")
    bool IsDisabled() const;
    UFUNCTION(BlueprintPure, Category = "PostHog")
    EPostHogSubsystemState GetState() const { return State; }
    UFUNCTION(BlueprintCallable, Category = "PostHog", meta = (AutoCreateRefTerm = "Properties"))
    EPostHogResult Capture(const FString& Event, const FPostHogProperties& Properties);
    /** Ok means SDK invocation, including SDK no-op cases such as duplicate identify. */
    UFUNCTION(BlueprintCallable, Category = "PostHog", meta = (AutoCreateRefTerm = "Set,SetOnce"))
    EPostHogResult Identify(const FString& DistinctId, const FPostHogProperties& Set, const FPostHogProperties& SetOnce);
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    EPostHogResult Reset();
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    EPostHogResult OptIn();
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    EPostHogResult OptOut();
    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagResult GetFeatureFlag(const FString& Key, EPostHogFlagReporting Reporting = EPostHogFlagReporting::Default);
    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlags GetFeatureFlags() const;
    /** Requests a refresh without returning a request-correlated future. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    EPostHogResult ReloadFeatureFlags();
    /** Automatic/restored/manual SDK updates, coalesced and delivered on the game thread. */
    UPROPERTY(BlueprintAssignable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagsChanged OnFeatureFlagsChanged;
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    EPostHogResult StartSessionReplay();
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    EPostHogResult StopSessionReplay();
    UFUNCTION(BlueprintPure, Category = "PostHog|Session Replay")
    bool IsSessionReplayActive() const;
    /** Reserve immediately before acquiring prepared pixels; inactive includes SDK capacity/policy refusal. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    EPostHogResult BeginReplayFrame(int32 Width, int32 Height, int32 RowStrideBytes, UPostHogReplayFrame*& Frame);

    /** Stable adapter for this initialized client. World lookup and binding are game-thread-only. */
    TSharedPtr<IAnalyticsProvider> GetAnalyticsProvider();
    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    bool BindAnalyticsProvider();
    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    void UnbindAnalyticsProvider();
    FPostHogClient::FPtr GetClient() const;
#if WITH_DEV_AUTOMATION_TESTS
    bool IsWaitingForPredecessorForTests() const;
    TWeakPtr<FPostHogInitialization, ESPMode::ThreadSafe> GetInitializationForTests() const;
    static void RetireClosedInitializationOwnersForTests();
#endif
    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogSetupCompleted OnSetupCompleted;
    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogShutdownCompleted OnShutdownCompleted;
private:
    friend class FPostHogModule;
    /** Module teardown, after all tracked initialization/native/host work has joined. */
    static void RetireInitializationOwners();
    bool Tick(float DeltaTime);
    void StartObserving();
    void StopObserving();
    void HandleEngineExit();
    void Stop();
    void EnsureTicker();
    EPostHogSubsystemState State = EPostHogSubsystemState::Idle;
    bool bForeground = true;
    bool bCaptureApplicationLifecycleEvents = true;
    FPostHogReplaySettings ReplaySettings;
    TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe> Recorder;
    FPostHogClient::FPtr Client;
    TSharedPtr<FPostHogAnalyticsProvider> AnalyticsProvider;
    TSharedPtr<FPostHogInitialization, ESPMode::ThreadSafe> Initialization;
    TSharedPtr<FPostHogLifecycleSignals, ESPMode::ThreadSafe> Signals;
    TSharedFuture<FPostHogDrainResult> Closing;
    FTSTicker::FDelegateHandle TickerHandle;
    FDelegateHandle BackgroundHandle, ForegroundHandle, DeactivateHandle, ReactivateHandle, SlateHandle, ExitHandle;
};
