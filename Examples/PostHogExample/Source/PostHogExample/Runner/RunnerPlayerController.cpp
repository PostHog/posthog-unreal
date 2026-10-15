#include "RunnerPlayerController.h"
#include "RunnerGameInstance.h"
#include "RunnerGameMode.h"
#include "RunnerMenu.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"

void ARunnerPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (!IsLocalController()) return;
	// Low beside the runway, looking down the track past the waiting runner.
	const FVector Eye(80.f, -330.f, 170.f);
	const FVector Focus(1500.f, 40.f, 160.f);
	MenuCamera = GetWorld()->SpawnActor<ACameraActor>(Eye, (Focus - Eye).Rotation());
	if (MenuCamera)
	{
		MenuCamera->GetCameraComponent()->bConstrainAspectRatio = false;
		MenuCamera->GetCameraComponent()->SetFieldOfView(75.f);
	}
	UpdateViewTarget();
}

void ARunnerPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();
	// Controller bindings run before the pawn's. Shared keys pass through so the runner still
	// jumps on Space and restarts on Space/Enter after game over; menu-only keys are consumed.
	const auto Bind = [this](FKey Key, void (ARunnerPlayerController::*Handler)(), bool bConsume)
	{
		FInputKeyBinding& Binding = InputComponent->BindKey(Key, IE_Pressed, this, Handler);
		Binding.bConsumeInput = bConsume;
		Binding.bExecuteWhenPaused = true;
	};
	Bind(EKeys::Escape, &ARunnerPlayerController::Back, true);
	Bind(EKeys::Enter, &ARunnerPlayerController::Confirm, false);
	Bind(EKeys::SpaceBar, &ARunnerPlayerController::Confirm, false);
	Bind(EKeys::Up, &ARunnerPlayerController::MenuUp, false);
	Bind(EKeys::W, &ARunnerPlayerController::MenuUp, false);
	Bind(EKeys::Down, &ARunnerPlayerController::MenuDown, false);
	Bind(EKeys::S, &ARunnerPlayerController::MenuDown, false);
}

void ARunnerPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	SyncMenu();
	UpdateViewTarget();
}

ARunnerGameMode* ARunnerPlayerController::GetRunMode() const
{
	return GetWorld() ? GetWorld()->GetAuthGameMode<ARunnerGameMode>() : nullptr;
}

int32 ARunnerPlayerController::GetMenuIndex() const
{
	const ARunnerGameMode* Run = GetRunMode();
	return Run && Run->Menu == IndexedMenu ? MenuIndex : 0;
}

void ARunnerPlayerController::SyncMenu()
{
	const ARunnerGameMode* Run = GetRunMode();
	if (!Run || Run->Menu == IndexedMenu) return;
	IndexedMenu = Run->Menu;
	MenuIndex = 0;
}

void ARunnerPlayerController::MenuUp()
{
	Navigate(-1);
}

void ARunnerPlayerController::MenuDown()
{
	Navigate(1);
}

void ARunnerPlayerController::Navigate(int32 Step)
{
	SyncMenu();
	MenuIndex = RunnerMenu::Wrap(MenuIndex + Step, IndexedMenu);
}

void ARunnerPlayerController::Confirm()
{
	ARunnerGameMode* Run = GetRunMode();
	if (!Run) return;
	SyncMenu();
	if (Run->Menu == ERunnerMenu::Title)
	{
		Run->ShowMainMenu();
		return;
	}
	const TConstArrayView<ERunnerMenuAction> Items = RunnerMenu::Items(Run->Menu);
	if (Items.IsValidIndex(MenuIndex)) Activate(Items[MenuIndex]);
}

void ARunnerPlayerController::Back()
{
	ARunnerGameMode* Run = GetRunMode();
	if (!Run) return;
	if (Run->Menu == ERunnerMenu::Main) Run->ShowTitle();
	else if (Run->bGameOver) Run->ReturnToMainMenu();
	else if (Run->Menu == ERunnerMenu::None || Run->Menu == ERunnerMenu::Pause) Run->TogglePause();
}

void ARunnerPlayerController::TogglePause()
{
	if (ARunnerGameMode* Run = GetRunMode()) Run->TogglePause();
}

void ARunnerPlayerController::Activate(ERunnerMenuAction Action)
{
	ARunnerGameMode* Run = GetRunMode();
	URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance());
	if (!Run || !Instance) return;
	switch (Action)
	{
	case ERunnerMenuAction::StartRun: Run->StartRun(); break;
	case ERunnerMenuAction::Resume: Run->SetPaused(false); break;
	case ERunnerMenuAction::Restart: Run->RestartRun(); break;
	case ERunnerMenuAction::ToggleAnalytics: Instance->SetAnalyticsEnabled(!Instance->IsAnalyticsEnabled()); break;
	case ERunnerMenuAction::ReloadFlags: Instance->ReloadFlags(); break;
	case ERunnerMenuAction::MainMenu: Run->ReturnToMainMenu(); break;
	case ERunnerMenuAction::Quit: Run->QuitGame(); break;
	}
}

void ARunnerPlayerController::UpdateViewTarget()
{
	const ARunnerGameMode* Run = GetRunMode();
	if (!Run || !MenuCamera) return;
	if (Run->IsInFrontMenu())
	{
		if (GetViewTarget() != MenuCamera) SetViewTargetWithBlend(MenuCamera, 0.f);
	}
	else if (GetViewTarget() == MenuCamera && GetPawn())
	{
		SetViewTargetWithBlend(GetPawn(), 1.2f, VTBlend_EaseInOut, 2.f);
	}
}
