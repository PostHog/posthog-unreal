#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PostHogSettings.h"
#include "PostHogSubsystem.generated.h"

UENUM(BlueprintType)
enum class EPostHogSubsystemState : uint8 { Idle, Initializing, Ready, ShuttingDown, Failed, Deinitialized };

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogSetupCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogShutdownCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogFeatureFlagsReloaded, const FPostHogFlagReloadResult&, Result);

struct FPostHogInitialization;
struct FPostHogLifecycleSignals;
class FPostHogAnalyticsProvider;
class IAnalyticsProvider;

/** One client per game instance. All UObject entry points run on the game thread. */
UCLASS()
class POSTHOG_API UPostHogSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()
public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    /** Begins asynchronous setup. False means rejected; completion is a separate event.
     * Active/initializing/stopping clients must finish shutdown before setup again. */
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    bool Setup(const FPostHogOptions& Options);
    /** C++ setup supports BeforeSend and custom plain-data context. Storage defaults
     * to Saved/PostHog; environment/PIE namespacing applies even with a custom root. */
    bool SetupWithConfig(FPostHogConfig Config, bool bEnableInEditor = false, bool bEnableOnDedicatedServer = false);
    UFUNCTION(BlueprintCallable, Category = "PostHog")
    void Shutdown();
    UFUNCTION(BlueprintPure, Category = "PostHog")
    bool IsInitialized() const;
    UFUNCTION(BlueprintPure, Category = "PostHog")
    EPostHogSubsystemState GetState() const { return State; }

    /** Returns queue admission, not a delivery receipt. Not-ready instances return Closed. */
    UFUNCTION(BlueprintCallable, Category = "PostHog", meta = (AutoCreateRefTerm = "Properties"))
    EPostHogCaptureResult Capture(const FString& Event, const FPostHogProperties& Properties);
    /** Updates local identity without waiting for the resulting flag refresh.
     * False includes rejected calls and duplicate identifies with no changes. */
    UFUNCTION(BlueprintCallable, Category = "PostHog", meta = (AutoCreateRefTerm = "Set,SetOnce"))
    bool Identify(const FString& DistinctId, const FPostHogProperties& Set, const FPostHogProperties& SetOnce);
    /** Reads the cache only. Missing differs from false; optionally records an exposure. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagResult GetFeatureFlag(const FString& Key, bool bSendEvent = true);
    /** Starts a refresh; bind OnFeatureFlagsReloaded first. False means not ready.
     * Calls while a refresh is pending share one completion. Shutdown cancels notification. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    bool ReloadFeatureFlags();

    /** Explicit reload completion on the game thread, not an automatic flag-change event. */
    UPROPERTY(BlueprintAssignable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagsReloaded OnFeatureFlagsReloaded;

    /** Same adapter for this initialized client; null while not ready. Game thread only. */
    TSharedPtr<IAnalyticsProvider> GetAnalyticsProvider();

    /** Designates this initialized instance for Unreal's world-less Analytics API.
     * Returns false if another live instance is bound. Does not change engine INI configuration. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    bool BindAnalyticsProvider();
    /** Releases only this instance's designation; existing adapter pointers remain instance-bound. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    void UnbindAnalyticsProvider();

    /** Native callers may retain the shared pointer, but teardown still closes it. */
    FPostHogClient::FPtr GetClient() const;
    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogSetupCompleted OnSetupCompleted;
    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogShutdownCompleted OnShutdownCompleted;

private:
    bool Tick(float DeltaTime);
    void StartObserving();
    void StopObserving();
    void HandleEngineExit();
    void Stop();
    void EnsureTicker();
    EPostHogSubsystemState State = EPostHogSubsystemState::Idle;
    bool bForeground = true;
    FPostHogClient::FPtr Client;
    TSharedPtr<FPostHogAnalyticsProvider> AnalyticsProvider;
    TSharedPtr<FPostHogInitialization, ESPMode::ThreadSafe> Initialization;
    TSharedPtr<FPostHogLifecycleSignals, ESPMode::ThreadSafe> Signals;
    TSharedFuture<bool> Closing;
    TSharedFuture<FPostHogFlagReloadResult> FlagReload;
    FTSTicker::FDelegateHandle TickerHandle;
    FDelegateHandle BackgroundHandle, ForegroundHandle, DeactivateHandle, ReactivateHandle, SlateHandle, ExitHandle;
};
