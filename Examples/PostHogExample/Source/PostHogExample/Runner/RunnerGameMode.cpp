#include "RunnerGameMode.h"
#include "RunnerCharacter.h"
#include "RunnerGameInstance.h"
#include "RunnerHUD.h"
#include "RunnerEnvironment.h"
#include "RunnerPlayerController.h"
#include "RunnerRules.h"
#include "RunnerTrackSegment.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerStart.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "PostHogSubsystem.h"

namespace
{
    const TCHAR* QuickStartOption = TEXT("quickstart");
    const TCHAR* MainMenuOption = TEXT("mainmenu");
}

ARunnerGameMode::ARunnerGameMode()
{
    PrimaryActorTick.bCanEverTick = true;
    DefaultPawnClass = ARunnerCharacter::StaticClass();
    PlayerControllerClass = ARunnerPlayerController::StaticClass();
    HUDClass = ARunnerHUD::StaticClass();
}

void ARunnerGameMode::InitGame(const FString& MapName, const FString& Options, FString& ErrorMessage)
{
    Super::InitGame(MapName, Options, ErrorMessage);
    if (UGameplayStatics::HasOption(Options, QuickStartOption)) Menu = ERunnerMenu::None;
    else if (UGameplayStatics::HasOption(Options, MainMenuOption)) Menu = ERunnerMenu::Main;
    // Build an entire playable scene in the engine's empty Entry map before the player spawns.
    RunnerStart = GetWorld()->SpawnActor<APlayerStart>(FVector(600, 0, 100), FRotator::ZeroRotator);
    RunnerEnvironment::Spawn(GetWorld());
    ExtendTrack(600.f);
}

AActor* ARunnerGameMode::ChoosePlayerStart_Implementation(AController* Player)
{
    // The runner always begins on its runway, independently of the editor camera position.
    return RunnerStart ? RunnerStart.Get() : Super::ChoosePlayerStart_Implementation(Player);
}

void ARunnerGameMode::BeginPlay()
{
    Super::BeginPlay();
    RunId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
    if (APawn* Pawn = UGameplayStatics::GetPlayerPawn(this, 0)) StartX = Pawn->GetActorLocation().X;
    OpenMenu(Menu);
}

void ARunnerGameMode::EndPlay(const EEndPlayReason::Type Reason)
{
    CaptureRunEnd(TEXT("abandoned"));
    Super::EndPlay(Reason);
}

float ARunnerGameMode::GetRunSpeed() const
{
    return FMath::Min(1800.f, RunnerRules::SpeedForDistance(DistanceMeters) + (bHardMode ? 150.f : 0.f));
}

void ARunnerGameMode::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    if (bGameOver) return;
    // The front menus hold the countdown, so the runner waits at the start until a run is chosen.
    if (!IsInFrontMenu()) Countdown = FMath::Max(0.f, Countdown - DeltaSeconds);
    if (IsRunning())
    {
        if (!bRunStarted)
        {
            bRunStarted = true;
            if (URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
            {
                // Snapshot the cached flag once per run; a late refresh affects the next run.
                bHardMode = Instance->UseHardMode();
                Instance->Capture(TEXT("run_started"), RunProperties());
            }
        }
        RunSeconds += DeltaSeconds;
    }
    if (APawn* Pawn = UGameplayStatics::GetPlayerPawn(this, 0))
    {
        const float PlayerX = Pawn->GetActorLocation().X;
        DistanceMeters = FMath::Max(0.f, (PlayerX - StartX) / 100.f);
        ExtendTrack(PlayerX);
    }
}

void ARunnerGameMode::ExtendTrack(float PlayerX)
{
    while (NextSegment * RunnerRules::SegmentLength < PlayerX + RunnerRules::LookAhead)
    {
        ARunnerTrackSegment* Segment = GetWorld()->SpawnActor<ARunnerTrackSegment>(
            FVector(NextSegment * RunnerRules::SegmentLength, 0.f, 0.f), FRotator::ZeroRotator);
        if (!Segment) break;
        Segment->Build(NextSegment);
        Segments.Add(Segment);
        ++NextSegment;
    }
    Segments.RemoveAll([PlayerX](ARunnerTrackSegment* Segment)
    {
        if (!IsValid(Segment)) return true;
        if (RunnerRules::ShouldRetire(Segment->GetActorLocation().X, PlayerX))
        {
            Segment->Destroy();
            return true;
        }
        return false;
    });
}

FPostHogProperties ARunnerGameMode::RunProperties() const
{
    FPostHogProperties Properties;
    Properties.SetString(TEXT("run_id"), RunId);
    Properties.SetString(TEXT("difficulty"), bHardMode ? TEXT("hard") : TEXT("normal"));
    Properties.SetNumber(TEXT("distance_meters"), DistanceMeters);
    Properties.SetNumber(TEXT("coins"), Coins);
    Properties.SetNumber(TEXT("duration_seconds"), RunSeconds);
    Properties.SetNumber(TEXT("speed_cm_per_second"), GetRunSpeed());
    if (const ARunnerCharacter* Player = Cast<ARunnerCharacter>(UGameplayStatics::GetPlayerPawn(this, 0)))
        Properties.SetNumber(TEXT("lane"), Player->TargetLane);
    return Properties;
}

void ARunnerGameMode::CaptureRunEnd(const FString& Reason)
{
    if (!bRunStarted || bEndCaptured) return;
    bEndCaptured = true;
    if (URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
    {
        auto Properties = RunProperties();
        Properties.SetString(TEXT("reason"), Reason);
        Instance->Capture(TEXT("run_ended"), Properties);
        if (auto Client = Instance->GetSubsystem<UPostHogSubsystem>()->GetClient()) Client->FlushAsync();
    }
}

void ARunnerGameMode::EndRun()
{
    if (!IsRunning()) return;
    bGameOver = true;
    CaptureRunEnd(TEXT("collision"));
    if (ARunnerCharacter* Player = Cast<ARunnerCharacter>(UGameplayStatics::GetPlayerPawn(this, 0)))
    {
        Player->GetCharacterMovement()->StopMovementImmediately();
        Player->GetCharacterMovement()->DisableMovement();
    }
    if (URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
    {
        Instance->RecordFinishedRun(FMath::FloorToInt(DistanceMeters), Coins);
        Instance->ViewScreen(TEXT("game_over"));
    }
}

void ARunnerGameMode::CollectCoin()
{
    if (!IsRunning()) return;
    ++Coins;
    if (URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
        Instance->Capture(TEXT("coin_collected"), RunProperties());
}

void ARunnerGameMode::RestartRun()
{
    if (bGameOver || Menu == ERunnerMenu::Pause) ReloadLevel(QuickStartOption);
}

void ARunnerGameMode::StartRun()
{
    if (IsInFrontMenu()) OpenMenu(ERunnerMenu::None);
}

void ARunnerGameMode::ShowMainMenu()
{
    if (Menu == ERunnerMenu::Title) OpenMenu(ERunnerMenu::Main);
}

void ARunnerGameMode::ShowTitle()
{
    if (Menu == ERunnerMenu::Main) OpenMenu(ERunnerMenu::Title);
}

void ARunnerGameMode::ReturnToMainMenu()
{
    // A fresh level discards the track and pawn; EndPlay reports any started run as abandoned.
    ReloadLevel(MainMenuOption);
}

void ARunnerGameMode::SetPaused(bool bPause)
{
    if (bLeavingLevel || bPause == (Menu == ERunnerMenu::Pause)) return;
    if (bPause && (Menu != ERunnerMenu::None || bGameOver)) return;
    if (!UGameplayStatics::SetGamePaused(this, bPause)) return;
    OpenMenu(bPause ? ERunnerMenu::Pause : ERunnerMenu::None);
}

void ARunnerGameMode::TogglePause()
{
    SetPaused(Menu != ERunnerMenu::Pause);
}

void ARunnerGameMode::QuitGame()
{
    UKismetSystemLibrary::QuitGame(this, UGameplayStatics::GetPlayerController(this, 0), EQuitPreference::Quit, false);
}

void ARunnerGameMode::OpenMenu(ERunnerMenu NewMenu)
{
    Menu = NewMenu;
    const TCHAR* Screen = NewMenu == ERunnerMenu::Title ? TEXT("title")
        : NewMenu == ERunnerMenu::Main ? TEXT("main_menu")
        : NewMenu == ERunnerMenu::Pause ? TEXT("pause") : nullptr;
    if (!Screen) return;
    if (URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance())) Instance->ViewScreen(Screen);
}

void ARunnerGameMode::ReloadLevel(const TCHAR* Options)
{
    // The first request wins, so ESC to the main menu cannot be overtaken by a same-frame restart.
    if (bLeavingLevel) return;
    bLeavingLevel = true;
    UGameplayStatics::SetGamePaused(this, false);
    UGameplayStatics::OpenLevel(this, FName(*UGameplayStatics::GetCurrentLevelName(this, true)), true, Options);
}
