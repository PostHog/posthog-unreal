#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "PostHogSettings.h"
#include "PostHogSubsystem.generated.h"

UENUM(BlueprintType)
enum class EPostHogSubsystemState : uint8
{
    Idle,
    Initializing,
    Ready,
    ShuttingDown,
    Failed,
    Deinitialized
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogSetupCompleted, bool, bSuccess);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogShutdownCompleted, const FPostHogDrainResult&, Result);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FPostHogFeatureFlagsChanged, const FPostHogFeatureFlagsUpdate&, Update);

class FPostHogReplayRecorder;
class FPostHogAnalyticsProvider;
class IAnalyticsProvider;

/** One stateful PostHog client per game instance. All UObject entry points run on the game thread. */
UCLASS()
class POSTHOG_API UPostHogSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    virtual void Initialize(FSubsystemCollectionBase& Collection) override;
    virtual void Deinitialize() override;

    UFUNCTION(BlueprintCallable, Category = "PostHog")
    bool Setup(const FPostHogOptions& Options);

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
    EPostHogCaptureResult Capture(const FString& Event, const FPostHogProperties& Properties);

    UFUNCTION(BlueprintCallable, Category = "PostHog", meta = (AutoCreateRefTerm = "Set,SetOnce"))
    void Identify(const FString& DistinctId, const FPostHogProperties& Set, const FPostHogProperties& SetOnce);

    UFUNCTION(BlueprintCallable, Category = "PostHog")
    void Reset();

    UFUNCTION(BlueprintCallable, Category = "PostHog")
    void OptIn();

    UFUNCTION(BlueprintCallable, Category = "PostHog")
    void OptOut();

    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagResult GetFeatureFlag(const FString& Key, EPostHogFlagReporting Reporting = EPostHogFlagReporting::Default);

    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlags GetFeatureFlags() const;

    UFUNCTION(BlueprintCallable, Category = "PostHog|Feature Flags")
    void ReloadFeatureFlags();

    UPROPERTY(BlueprintAssignable, Category = "PostHog|Feature Flags")
    FPostHogFeatureFlagsChanged OnFeatureFlagsChanged;

    /**
     * Starts viewport session replay (requires server-side remote config permission).
     * Note: viewport replay currently captures unredacted pixels (no UI/text masking is applied).
     */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    void StartSessionReplay();

    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    void StopSessionReplay();

    UFUNCTION(BlueprintPure, Category = "PostHog|Session Replay")
    bool IsSessionReplayActive() const;

    TSharedPtr<IAnalyticsProvider> GetAnalyticsProvider();

    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    bool BindAnalyticsProvider();

    UFUNCTION(BlueprintCallable, Category = "PostHog|Analytics")
    void UnbindAnalyticsProvider();

    FPostHogClient::FPtr GetClient() const;

    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogSetupCompleted OnSetupCompleted;

    UPROPERTY(BlueprintAssignable, Category = "PostHog")
    FPostHogShutdownCompleted OnShutdownCompleted;

private:
    void HandleLifecycleChange(bool bNextForeground);
    void StartObserving();
    void StopObserving();
    void ShutdownInternal(bool bDeinitializing);
    void FinishShutdown(TSharedPtr<struct FPostHogNamespaceLease, ESPMode::ThreadSafe> Lease, uint64 Generation, const FPostHogDrainResult& Result);
    void Stop();

    EPostHogSubsystemState State = EPostHogSubsystemState::Idle;
    uint64 SetupGeneration = 0;
    FString NamespaceKey;
    TSharedPtr<struct FPostHogNamespaceLease, ESPMode::ThreadSafe> ActiveLease;
    TArray<FPostHogFeatureFlagsUpdate> PendingFlagUpdates;
    bool bForeground = true;
    bool bCaptureApplicationLifecycleEvents = true;
    FPostHogReplaySettings ReplaySettings;
    FPostHogClient::FPtr Client;
    TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe> Recorder;
    TSharedPtr<FPostHogAnalyticsProvider> AnalyticsProvider;
    FDelegateHandle BackgroundHandle, ForegroundHandle, DeactivateHandle, ReactivateHandle, SlateHandle, ExitHandle;
};
