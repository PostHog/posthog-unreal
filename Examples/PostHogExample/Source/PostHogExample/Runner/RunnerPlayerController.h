#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "RunnerPlayerController.generated.h"

class ACameraActor;
class ARunnerGameMode;
enum class ERunnerMenu : uint8;
enum class ERunnerMenuAction : uint8;

/** Menu input and the front-menu camera. Gameplay input stays on ARunnerCharacter. */
UCLASS()
class ARunnerPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void SetupInputComponent() override;
	virtual void PlayerTick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void MenuUp();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void MenuDown();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void Confirm();
	/** Escape: pause/resume during a run, back on the main menu, main menu after game over. */
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void Back();
	UFUNCTION(BlueprintCallable, Category="Runner|Menu")
	void TogglePause();
	UFUNCTION(BlueprintPure, Category="Runner|Menu")
	int32 GetMenuIndex() const;

private:
	UPROPERTY()
	TObjectPtr<ACameraActor> MenuCamera;

	int32 MenuIndex = 0;
	ERunnerMenu IndexedMenu{};

	ARunnerGameMode* GetRunMode() const;
	void SyncMenu();
	void Navigate(int32 Step);
	void Activate(ERunnerMenuAction Action);
	void UpdateViewTarget();
};
