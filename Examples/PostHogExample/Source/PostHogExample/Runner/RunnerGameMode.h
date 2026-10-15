#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "PostHogProperties.h"
#include "RunnerGameMode.generated.h"

class ARunnerTrackSegment;
class APlayerStart;

/** Owns run state and a bounded window of procedurally generated track. */
UCLASS()
class ARunnerGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	ARunnerGameMode();
	virtual void InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage) override;
	virtual void BeginPlay() override;
	virtual AActor* ChoosePlayerStart_Implementation(AController* Player) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;

	UFUNCTION(BlueprintPure, Category="Runner")
	bool IsRunning() const { return Countdown <= 0.f && !bGameOver; }
	UFUNCTION(BlueprintPure, Category="Runner")
	float GetRunSpeed() const;
	UFUNCTION(BlueprintCallable, Category="Runner")
	void EndRun();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void CollectCoin();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void RestartRun();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	float Countdown = 3.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	float DistanceMeters = 0.f;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	int32 Coins = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	bool bGameOver = false;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	bool bHardMode = false;

private:
	UPROPERTY()
	TArray<TObjectPtr<ARunnerTrackSegment>> Segments;
	UPROPERTY()
	TObjectPtr<APlayerStart> RunnerStart;

	float StartX = 0.f;
	int32 NextSegment = 0;
	bool bRunStarted = false;
	bool bEndCaptured = false;
	float RunSeconds = 0.f;
	FString RunId;
	void ExtendTrack(float PlayerX);
	void CaptureRunEnd(const FString& Reason);
	FPostHogProperties RunProperties() const;
};
