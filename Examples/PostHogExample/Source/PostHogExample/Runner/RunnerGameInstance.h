#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "PostHogClient.h"
#include "RunnerGameInstance.generated.h"

class UPostHogSubsystem;

/** The subsystem owns the client; this example instance keeps early gameplay events until setup completes. */
UCLASS()
class URunnerGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override;
    virtual void Shutdown() override;
    void Capture(const FString& Event, const FPostHogProperties& Properties);
    bool UseHardMode() const;
    FString AnalyticsStatus() const;

    /** Screen views are menu chrome, not gameplay, so they are dropped rather than buffered before setup. */
    void ViewScreen(const FString& Name);
    UFUNCTION(BlueprintPure, Category="Runner|Analytics")
    bool HasClient() const;
    UFUNCTION(BlueprintPure, Category="Runner|Analytics")
    bool IsAnalyticsEnabled() const;
    UFUNCTION(BlueprintCallable, Category="Runner|Analytics")
    void SetAnalyticsEnabled(bool bEnabled);
    UFUNCTION(BlueprintCallable, Category="Runner|Analytics")
    void ReloadFlags();
    FString FlagReloadStatus() const;
    /** Reads the cached difficulty flag without sending a $feature_flag_called event. */
    FString DifficultyFlagLabel() const;
    FString ShortDistinctId() const;
    FString ShortSessionId() const;

    /** Display-only score shown by the HUD; it does not affect gameplay or analytics. */
    static int32 ScoreFor(int32 Meters, int32 Relics) { return Meters + Relics * 25; }
    /** Session-only bests kept in memory for the game-over panel. */
    void RecordFinishedRun(int32 Meters, int32 Relics);
    int32 GetBestDistance() const { return BestDistance; }
    int32 GetBestScore() const { return BestScore; }
    bool IsLastRunBest() const { return bLastRunBest; }

private:
    struct FPendingEvent { FString Name; FPostHogProperties Properties; };
    TArray<FPendingEvent> Pending;
    TOptional<TSharedFuture<FPostHogFlagReloadResult>> FlagReload;
    int32 BestDistance = 0;
    int32 BestScore = 0;
    bool bLastRunBest = false;
    FPostHogClient::FPtr GetClient() const;
    UPROPERTY()
    TObjectPtr<UPostHogSubsystem> PostHog;
    UFUNCTION()
    void SetupCompleted(bool bSuccess);
};
