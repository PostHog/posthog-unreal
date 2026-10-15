#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogSubsystem.h"
#include "PostHogJson.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace
{
struct FSubsystemStats
{
    std::atomic<int32> Backgrounds{0}, Opens{0}, Installs{0}, Updates{0};
    std::atomic<bool> OffThread{false};
};
struct FCreationBarrier
{
    FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
    ~FCreationBarrier() { FPlatformProcess::ReturnSynchEventToPool(Entered); FPlatformProcess::ReturnSynchEventToPool(Release); }
};

class FSubsystemScenario final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        PreviousAuto = Settings->bAutoInitialize; PreviousOptions = Settings->Options;
        Settings->bAutoInitialize = false;
        Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString());
        Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = Root; Config.bPreloadFeatureFlags = false; Config.FlushIntervalMilliseconds = 60000; Config.FlushAt = 100;
        Config.Context.SetString(TEXT("test_suite"), TEXT("subsystem"));
        Config.Context.SetString(TEXT("$app_version"), TEXT("1.0")); Config.Context.SetString(TEXT("$app_build"), TEXT("100"));
        Stats = MakeShared<FSubsystemStats, ESPMode::ThreadSafe>();
        Config.BeforeSend = [Counters = Stats](FPostHogEvent& Event)
        {
            if (Event.Event.StartsWith(TEXT("Application ")) && !IsInGameThread()) { Counters->OffThread = true; }
            if (Event.Event == TEXT("Application Installed")) { ++Counters->Installs; }
            if (Event.Event == TEXT("Application Updated")) { ++Counters->Updates; }
            if (Event.Event == TEXT("Application Opened")) { ++Counters->Opens; }
            if (Event.Event == TEXT("Application Backgrounded")) { ++Counters->Backgrounds; }
            return true;
        };
        Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FSubsystemScenario()
    {
        if (Barrier) { Barrier->Release->Trigger(); }
        Destroy(One); Destroy(Two); Destroy(Cancelled); Destroy(Replacement); Destroy(Automatic);
        auto* Settings = GetMutableDefault<UPostHogSettings>(); Settings->bAutoInitialize = PreviousAuto; Settings->Options = PreviousOptions;
    }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("subsystem scenario timed out")); return true; }
        switch (Phase)
        {
        case 0:
        {
            if (Config.Host.IsEmpty()) { Test->AddError(TEXT("Set POSTHOG_TEST_HOST")); return true; }
            One = Create(); Two = Create();
            auto* A = One->GetSubsystem<UPostHogSubsystem>(); auto* B = Two->GetSubsystem<UPostHogSubsystem>();
            Check(A && B, TEXT("game-instance subsystems created")); if (!A || !B) { return true; }
            Check(!A->SetupWithConfig(Config), TEXT("editor guard defaults off"));
            Two->GetWorldContext()->RunAsDedicated = true;
            Check(!B->SetupWithConfig(Config, true), TEXT("dedicated-server guard defaults off"));
            Two->GetWorldContext()->RunAsDedicated = false;
            auto First = Config; First.ClientName = TEXT("one"); First.Context.SetString(TEXT("instance"), TEXT("one"));
            auto Second = Config; Second.ClientName = TEXT("two"); Second.Context.SetString(TEXT("instance"), TEXT("two"));
            Check(A->SetupWithConfig(First, true) && B->SetupWithConfig(Second, true), TEXT("start two isolated owners"));
            Check(!A->SetupWithConfig(First, true), TEXT("repeated setup rejected while initializing"));
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast();
            ++Phase; return false;
        }
        case 1:
        {
            auto* A = One->GetSubsystem<UPostHogSubsystem>(); auto* B = Two->GetSubsystem<UPostHogSubsystem>();
            if (!A->IsInitialized() || !B->IsInitialized()) { return false; }
            FirstClient = A->GetClient(); SecondClient = B->GetClient();
            Check(FirstClient != SecondClient && FirstClient->GetDistinctId() != SecondClient->GetDistinctId(), TEXT("game-instance namespaces isolate native identity"));
            Check(Stats->Installs == 2 && Stats->Opens == 2 && !Stats->OffThread, TEXT("initial lifecycle emitted on game thread once per owner"));
            const FString Identity = FirstClient->GetDistinctId();
            UWorld* PreviousWorld = One->GetWorld();
            auto* NextWorld = UWorld::CreateWorld(EWorldType::Game, false, FName(*FGuid::NewGuid().ToString()));
            NextWorld->SetGameInstance(One.Get()); One->GetWorldContext()->SetCurrentWorld(NextWorld);
            PreviousWorld->SetGameInstance(nullptr); PreviousWorld->DestroyWorld(false);
            Check(One->GetSubsystem<UPostHogSubsystem>() == A && A->GetClient()->GetDistinctId() == Identity, TEXT("world replacement keeps game-instance client"));
            FirstClient->Capture(TEXT("subsystem_one")); SecondClient->Capture(TEXT("subsystem_two"));
            FCoreDelegates::ApplicationWillDeactivateDelegate.Broadcast(); FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Broadcast();
            ++Phase; return false;
        }
        case 2:
            if (Stats->Backgrounds != 2) { return false; }
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast(); FCoreDelegates::ApplicationHasReactivatedDelegate.Broadcast();
            ++Phase; return false;
        case 3:
            if (Stats->Opens != 4) { return false; }
            Reset = FirstClient->ResetAsync(); ++Phase; return false;
        case 4:
            if (!Reset.IsReady()) { return false; }
            Check(Reset.Get(), TEXT("identity reset persists while preserving app markers"));
            One->GetSubsystem<UPostHogSubsystem>()->Shutdown(); Two->GetSubsystem<UPostHogSubsystem>()->Shutdown();
            Check(FirstClient->Capture(TEXT("subsystem_after_shutdown")) == EPostHogCaptureResult::Closed, TEXT("retained native pointer closes with owner"));
            ++Phase; return false;
        case 5:
        {
            if (One->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle || Two->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Check(Stats->Backgrounds == 2 && Stats->Opens == 4, TEXT("duplicate platform signals suppressed"));
            auto Updated = Config; Updated.ClientName = TEXT("one"); Updated.Context.SetString(TEXT("instance"), TEXT("one-updated"));
            Updated.Context.SetString(TEXT("$app_version"), TEXT("1.1")); Updated.Context.SetString(TEXT("$app_build"), TEXT("110"));
            Check(One->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Updated, true), TEXT("restart same namespace"));
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast(); ++Phase; return false;
        }
        case 6:
        {
            if (!One->GetSubsystem<UPostHogSubsystem>()->IsInitialized()) { return false; }
            Check(Stats->Installs == 2 && Stats->Updates == 1, TEXT("reset/restart preserves install marker and emits update"));
            One->GetSubsystem<UPostHogSubsystem>()->Shutdown(); ++Phase; return false;
        }
        case 7:
        {
            if (One->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Destroy(One); Destroy(Two); FirstClient.Reset(); SecondClient.Reset();
            Barrier = MakeShared<FCreationBarrier, ESPMode::ThreadSafe>();
            Cancelled = Create(); auto Blocked = Config;
            Blocked.Now = [Gate = Barrier] { Gate->Entered->Trigger(); Gate->Release->Wait(); return FDateTime::UtcNow(); };
            Check(Cancelled->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Blocked, true), TEXT("begin controlled initialization"));
            ++Phase; return false;
        }
        case 8:
        {
            if (!Barrier->Entered->Wait(0)) { return false; }
            TWeakObjectPtr<UGameInstance> Weak = Cancelled.Get(); Destroy(Cancelled);
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Check(!Weak.IsValid(), TEXT("initializing game instance can be garbage collected"));
            Replacement = Create();
            Check(Replacement->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Config, true), TEXT("new owner queues same namespace during cancelled initialization"));
            ++Phase; return false;
        }
        case 9:
            Check(!Replacement->GetSubsystem<UPostHogSubsystem>()->IsInitialized(), TEXT("new owner cannot overtake old initializer"));
            Barrier->Release->Trigger(); FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast(); ++Phase; return false;
        case 10:
            if (!Replacement->GetSubsystem<UPostHogSubsystem>()->IsInitialized()) { return false; }
            Replacement->GetSubsystem<UPostHogSubsystem>()->GetClient()->Capture(TEXT("subsystem_replacement"));
            Replacement->GetSubsystem<UPostHogSubsystem>()->Shutdown(); ++Phase; return false;
        case 11:
        {
            if (Replacement->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Destroy(Replacement);
            auto* Settings = GetMutableDefault<UPostHogSettings>(); Settings->bAutoInitialize = true;
            Settings->Options = {}; Settings->Options.ProjectToken = TEXT("phc_MOCK"); Settings->Options.Host = Config.Host;
            Settings->Options.bEnableInEditor = true; Settings->Options.bPreloadFeatureFlags = false;
            Settings->Options.bCaptureApplicationLifecycleEvents = false;
            Settings->Options.ClientName = TEXT("automation-") + FGuid::NewGuid().ToString();
            AutomaticStorage = FPaths::ProjectSavedDir() / TEXT("PostHog") / FMD5::HashAnsiString(*(Config.Host + TEXT("\nphc_MOCK\neditor/standalone/") + Settings->Options.ClientName));
            Automatic = Create(); ++Phase; return false;
        }
        case 12:
            if (!Automatic->GetSubsystem<UPostHogSubsystem>()->IsInitialized()) { return false; }
            Automatic->GetSubsystem<UPostHogSubsystem>()->GetClient()->Capture(TEXT("subsystem_auto"));
            Automatic->GetSubsystem<UPostHogSubsystem>()->Shutdown(); ++Phase; return false;
        case 13:
            if (Automatic->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Check(!Automatic->GetSubsystem<UPostHogSubsystem>()->Setup(FPostHogOptions()), TEXT("explicit setup does not inherit project credentials"));
            Destroy(Automatic);
            FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Broadcast(); FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast();
            Check(!Stats->OffThread, TEXT("lifecycle hooks stay on game thread"));
            IFileManager::Get().DeleteDirectory(*Root, false, true);
            IFileManager::Get().DeleteDirectory(*AutomaticStorage, false, true);
            return true;
        }
        return false;
    }
private:
    TStrongObjectPtr<UGameInstance> Create()
    {
        TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
        Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
        return Instance;
    }
    void Destroy(TStrongObjectPtr<UGameInstance>& Instance)
    {
        if (!Instance) { return; }
        UWorld* World = Instance->GetWorld(); Instance->Shutdown();
        if (World) { World->SetGameInstance(nullptr); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        Instance.Reset();
    }
    void Check(bool Pass, const TCHAR* Message) { if (!Pass) { Test->AddError(Message); } }
    FAutomationTestBase* Test;
    FPostHogConfig Config;
    FString Root, AutomaticStorage;
    FPostHogOptions PreviousOptions;
    bool PreviousAuto = false;
    int32 Phase = 0;
    double Deadline = 0;
    TStrongObjectPtr<UGameInstance> One, Two, Cancelled, Replacement, Automatic;
    FPostHogClient::FPtr FirstClient, SecondClient;
    TSharedFuture<bool> Reset;
    TSharedPtr<FSubsystemStats, ESPMode::ThreadSafe> Stats;
    TSharedPtr<FCreationBarrier, ESPMode::ThreadSafe> Barrier;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemTest, "PostHog.Subsystem",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogSubsystemTest::RunTest(const FString& Parameters)
{
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FSubsystemScenario>(this));
    return true;
}
#endif
