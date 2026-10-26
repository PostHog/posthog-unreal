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

struct FPostHogInitialization;
struct FPostHogLifecycleSignals;
class FPostHogReplayRecorder;

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

    /** Starts the recorder after setup; requires Replay.bEnabled. Already started returns true.
     * True means the recorder is running, not that frames are being captured or delivered:
     * consent, foreground/session state, and remote replay configuration still gate capture. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    bool StartSessionReplay();

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
    void StopReplay();
    EPostHogSubsystemState State = EPostHogSubsystemState::Idle;
    bool bForeground = true;
    FPostHogClient::FPtr Client;
    /** Replay options kept from setup for automatic or explicit recorder startup. */
    FPostHogReplaySettings ReplaySettings;
    TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe> Replay;
    TSharedPtr<FPostHogInitialization, ESPMode::ThreadSafe> Initialization;
    TSharedPtr<FPostHogLifecycleSignals, ESPMode::ThreadSafe> Signals;
    TSharedFuture<bool> Closing;
    FTSTicker::FDelegateHandle TickerHandle;
    FDelegateHandle BackgroundHandle, ForegroundHandle, DeactivateHandle, ReactivateHandle, SlateHandle, ExitHandle;
};
