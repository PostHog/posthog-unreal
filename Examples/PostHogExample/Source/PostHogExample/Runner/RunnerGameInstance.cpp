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
    if (PostHog->IsInitialized()) return TEXT("ANALYTICS READY");
    if (PostHog->GetState() == EPostHogSubsystemState::Initializing) return TEXT("ANALYTICS CONNECTING");
    if (PostHog->GetState() == EPostHogSubsystemState::Failed) return TEXT("ANALYTICS FAILED");
    return TEXT("ANALYTICS OFFLINE DEMO");
}
