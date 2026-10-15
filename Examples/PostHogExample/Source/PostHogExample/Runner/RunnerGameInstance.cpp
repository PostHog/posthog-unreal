#include "RunnerGameInstance.h"
#include "PostHogSubsystem.h"
#include "HAL/PlatformMisc.h"

void URunnerGameInstance::Init()
{
    Super::Init();
    PostHog = GetSubsystem<UPostHogSubsystem>();
    PostHog->OnSetupCompleted.AddDynamic(this, &URunnerGameInstance::SetupCompleted);
    // Explicit environment configuration enables analytics for this example, including PIE.
    // A client already configured through Project Settings retains its normal subsystem ownership.
    const FString Token = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TOKEN"));
    if (!Token.IsEmpty() && PostHog->GetState() == EPostHogSubsystemState::Idle)
    {
        FPostHogOptions Options;
        Options.ProjectToken = Token;
        const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_HOST"));
        if (!Host.IsEmpty()) Options.Host = Host;
        Options.ClientName = TEXT("runner-example");
        Options.bEnableInEditor = true;
        Options.FlushIntervalMilliseconds = 1000;
        PostHog->Setup(Options);
    }
}

void URunnerGameInstance::Shutdown()
{
    if (PostHog) PostHog->OnSetupCompleted.RemoveDynamic(this, &URunnerGameInstance::SetupCompleted);
    Pending.Empty();
    FlagReload.Reset();
    // UPostHogSubsystem handles shutdown when the game instance's subsystems deinitialize.
    Super::Shutdown();
}

void URunnerGameInstance::Capture(const FString& Event, const FPostHogProperties& Properties)
{
    if (!PostHog) return;
    if (auto Client = PostHog->GetClient()) Client->Capture(Event, Properties);
    else if (PostHog->GetState() == EPostHogSubsystemState::Initializing && Pending.Num() < 64)
        Pending.Add({Event, Properties});
}

void URunnerGameInstance::SetupCompleted(bool bSuccess)
{
    if (auto Client = PostHog->GetClient(); bSuccess && Client)
        for (const FPendingEvent& Event : Pending) Client->Capture(Event.Name, Event.Properties);
    Pending.Empty();
}

bool URunnerGameInstance::UseHardMode() const
{
    if (auto Client = PostHog->GetClient())
        return Client->IsFeatureEnabled(TEXT("runner-hard-mode"), true, false);
    return false;
}

FString URunnerGameInstance::AnalyticsStatus() const
{
    if (PostHog->IsInitialized()) return IsAnalyticsEnabled() ? TEXT("ANALYTICS READY") : TEXT("ANALYTICS OPTED OUT");
    if (PostHog->GetState() == EPostHogSubsystemState::Initializing) return TEXT("ANALYTICS CONNECTING");
    if (PostHog->GetState() == EPostHogSubsystemState::Failed) return TEXT("ANALYTICS FAILED");
    return TEXT("ANALYTICS OFFLINE DEMO");
}

void URunnerGameInstance::ViewScreen(const FString& Name)
{
    if (auto Client = GetClient()) Client->Screen(Name);
}

FPostHogClient::FPtr URunnerGameInstance::GetClient() const
{
    return PostHog ? PostHog->GetClient() : FPostHogClient::FPtr();
}

bool URunnerGameInstance::HasClient() const
{
    return GetClient().IsValid();
}

bool URunnerGameInstance::IsAnalyticsEnabled() const
{
    auto Client = GetClient();
    return Client && !Client->IsOptedOut();
}

void URunnerGameInstance::SetAnalyticsEnabled(bool bEnabled)
{
    auto Client = GetClient();
    if (!Client) return;
    if (bEnabled) Client->OptInAsync();
    else Client->OptOutAsync();
}

void URunnerGameInstance::ReloadFlags()
{
    if (auto Client = GetClient()) FlagReload = Client->ReloadFeatureFlagsAsync();
}

FString URunnerGameInstance::FlagReloadStatus() const
{
    if (!FlagReload) return FString();
    if (!FlagReload->IsReady()) return TEXT("FLAGS RELOADING...");
    const EPostHogFlagReloadStatus Status = FlagReload->Get().Status;
    return Status == EPostHogFlagReloadStatus::Success || Status == EPostHogFlagReloadStatus::Partial
        ? TEXT("FLAGS RELOADED") : TEXT("FLAG RELOAD FAILED");
}

FString URunnerGameInstance::DifficultyFlagLabel() const
{
    auto Client = GetClient();
    if (!Client) return TEXT("UNKNOWN");
    // Mirrors IsFeatureEnabled: true or any variant string enables hard mode.
    const FString Json = Client->GetFeatureFlag(TEXT("runner-hard-mode"), false).ToJson();
    if (Json == TEXT("null")) return TEXT("UNKNOWN");
    return Json == TEXT("true") || Json.StartsWith(TEXT("\"")) ? TEXT("HARD") : TEXT("NORMAL");
}

FString URunnerGameInstance::ShortDistinctId() const
{
    auto Client = GetClient();
    return Client ? Client->GetDistinctId().Left(8) : FString();
}

FString URunnerGameInstance::ShortSessionId() const
{
    auto Client = GetClient();
    return Client ? Client->GetSessionId().Left(8) : FString();
}

void URunnerGameInstance::RecordFinishedRun(int32 Meters, int32 Relics)
{
    const int32 Score = ScoreFor(Meters, Relics);
    bLastRunBest = Score > BestScore;
    BestScore = FMath::Max(BestScore, Score);
    BestDistance = FMath::Max(BestDistance, Meters);
}
