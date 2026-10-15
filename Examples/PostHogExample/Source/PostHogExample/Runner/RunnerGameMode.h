#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "PostHogProperties.h"
#include "RunnerGameMode.generated.h"

class ARunnerTrackSegment;
class APlayerStart;

UENUM(BlueprintType)
enum class ERunnerMenu : uint8 { None, Title, Main, Pause };

/** Owns run state, the menu flow, and a bounded window of procedurally generated track. */
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
	bool IsInFrontMenu() const { return Menu == ERunnerMenu::Title || Menu == ERunnerMenu::Main; }
	UFUNCTION(BlueprintPure, Category="Runner")
	float GetRunSpeed() const;
	UFUNCTION(BlueprintCallable, Category="Runner")
	void EndRun();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void CollectCoin();
	/** Reloads straight into a new countdown; only after game over or from the pause menu. */
	UFUNCTION(BlueprintCallable, Category="Runner")
	void RestartRun();

	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void StartRun();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void ShowMainMenu();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void ShowTitle();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void ReturnToMainMenu();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void SetPaused(bool bPause);
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void TogglePause();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void QuitGame();

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
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner|Menu")
	ERunnerMenu Menu = ERunnerMenu::Title;

private:
	UPROPERTY()
	TArray<TObjectPtr<ARunnerTrackSegment>> Segments;
	UPROPERTY()
	TObjectPtr<APlayerStart> RunnerStart;

	float StartX = 0.f;
	int32 NextSegment = 0;
	bool bRunStarted = false;
	bool bEndCaptured = false;
	bool bLeavingLevel = false;
	float RunSeconds = 0.f;
	FString RunId;
	void ExtendTrack(float PlayerX);
	void CaptureRunEnd(const FString& Reason);
	FPostHogProperties RunProperties() const;
	void OpenMenu(ERunnerMenu NewMenu);
	void ReloadLevel(const TCHAR* Options);
};
