#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace
{
TStrongObjectPtr<UGameInstance> CreateTestGameInstance()
{
    TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
    FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
    Context.OwningGameInstance = Instance.Get();
    UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
    Context.SetCurrentWorld(World);
    Instance->Init();
    return Instance;
}

void DestroyTestGameInstance(TStrongObjectPtr<UGameInstance>& Instance)
{
    if (Instance.IsValid())
    {
        Instance->Shutdown();
        if (GEngine) { GEngine->DestroyWorldContext(Instance->GetWorld()); }
        Instance.Reset();
    }
}

class FSubsystemLifecycleLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemLifecycleLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        Deadline = FPlatformTime::Seconds() + 15.0;
    }

    ~FSubsystemLifecycleLatent()
    {
        DestroyTestGameInstance(Instance);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(TEXT("Subsystem lifecycle test timed out"));
            return true;
        }
        if (Phase == 0)
        {
            Instance = CreateTestGameInstance();
            auto* Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestNotNull(TEXT("subsystem exists"), Subsystem);
            if (!Subsystem) { return true; }

            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_subsystem");
            Config.Host = TEXT("http://127.0.0.1:9");
            Config.StorageRoot = Root;
            Config.bPreloadFeatureFlags = false;
            Config.ShutdownTimeoutMilliseconds = 500;
            Test->TestTrue(TEXT("setup accepted"), Subsystem->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Subsystem->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("subsystem ready"), Subsystem->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("initialized"), Subsystem->IsInitialized());
            Test->TestEqual(TEXT("capture accepted"), Subsystem->Capture(TEXT("sub_event"), {}), EPostHogCaptureResult::Accepted);
            Subsystem->Shutdown();
            Phase = 2;
            return false;
        }
        if (Subsystem->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("subsystem idle after shutdown"), Subsystem->GetState(), EPostHogSubsystemState::Idle);
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    FString Root;
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};

class FSubsystemCancelDuringInitLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemCancelDuringInitLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        ClientName = TEXT("cancel-init-") + FGuid::NewGuid().ToString();
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FSubsystemCancelDuringInitLatent()
    {
        DestroyTestGameInstance(Instance2);
        DestroyTestGameInstance(Instance1);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    FPostHogConfig MakeConfig() const
    {
        FPostHogConfig Config;
        Config.ProjectToken = TEXT("phc_test_cancel_init");
        Config.Host = TEXT("http://127.0.0.1:9");
        Config.StorageRoot = Root;
        Config.ClientName = ClientName;
        Config.bPreloadFeatureFlags = false;
        Config.ShutdownTimeoutMilliseconds = 500;
        return Config;
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Cancel-during-init test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Instance1 = CreateTestGameInstance();
            auto* Sub1 = Instance1->GetSubsystem<UPostHogSubsystem>();
            Test->TestTrue(TEXT("initial setup accepted"), Sub1->SetupWithConfig(MakeConfig(), true, true));
            Phase = 1;
            return false;
        }
        if (Phase == 1)
        {
            auto* Sub1 = Instance1->GetSubsystem<UPostHogSubsystem>();
            if (Sub1->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("initial setup ready"), Sub1->GetState(), EPostHogSubsystemState::Ready);
            Sub1->Identify(TEXT("persisted-player-99"), {}, {});
            ExpectedDistinctId = Sub1->GetClient()->GetDistinctId();
            Test->TestEqual(TEXT("identified distinct id"), ExpectedDistinctId, FString(TEXT("persisted-player-99")));
            Sub1->Shutdown();
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            auto* Sub1 = Instance1->GetSubsystem<UPostHogSubsystem>();
            if (Sub1->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            Test->TestEqual(TEXT("sub1 idle before cancel-init"), Sub1->GetState(), EPostHogSubsystemState::Idle);

            // Start init on Sub1, immediately cancel while Initializing, and immediately start Sub2
            // on the exact same storage namespace so Sub2 must wait on Sub1's namespace lease.
            Test->TestTrue(TEXT("second setup on sub1 started"), Sub1->SetupWithConfig(MakeConfig(), true, true));
            Test->TestEqual(TEXT("sub1 initializing"), Sub1->GetState(), EPostHogSubsystemState::Initializing);
            Sub1->Shutdown();
            Test->TestTrue(TEXT("sub1 transitions to shutting down (or idle if already finished)"),
                Sub1->GetState() == EPostHogSubsystemState::ShuttingDown || Sub1->GetState() == EPostHogSubsystemState::Idle);

            Instance2 = CreateTestGameInstance();
            auto* Sub2 = Instance2->GetSubsystem<UPostHogSubsystem>();
            Test->TestTrue(TEXT("replacement setup on sub2 accepted"), Sub2->SetupWithConfig(MakeConfig(), true, true));
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            auto* Sub1 = Instance1->GetSubsystem<UPostHogSubsystem>();
            auto* Sub2 = Instance2->GetSubsystem<UPostHogSubsystem>();
            if (Sub1->GetState() == EPostHogSubsystemState::ShuttingDown || Sub2->GetState() == EPostHogSubsystemState::Initializing)
            {
                return false;
            }
            Test->TestEqual(TEXT("sub1 idle after cancel"), Sub1->GetState(), EPostHogSubsystemState::Idle);
            Test->TestEqual(TEXT("sub2 ready after waiting on lease"), Sub2->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("sub2 initialized"), Sub2->IsInitialized());
            if (auto Client2 = Sub2->GetClient())
            {
                Test->TestEqual(TEXT("persisted distinct_id preserved"), Client2->GetDistinctId(), ExpectedDistinctId);
                ShutdownFuture = Client2->ShutdownAsync();
            }
            Sub2->Shutdown();
            Phase = 4;
            return false;
        }
        auto* Sub2 = Instance2->GetSubsystem<UPostHogSubsystem>();
        if (Sub2->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("sub2 idle"), Sub2->GetState(), EPostHogSubsystemState::Idle);
        if (ShutdownFuture.IsValid())
        {
            const auto Drain = ShutdownFuture.Get();
            Test->TestEqual(TEXT("sub2 persisted cleanly without lock contention"),
                Drain.State, EPostHogPersistenceOutcome::Persisted);
        }
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance1;
    TStrongObjectPtr<UGameInstance> Instance2;
    FString Root;
    FString ClientName;
    FString ExpectedDistinctId;
    TSharedFuture<FPostHogDrainResult> ShutdownFuture;
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};

class FSubsystemDisabledAndLifecycleLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemDisabledAndLifecycleLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FSubsystemDisabledAndLifecycleLatent()
    {
        DestroyTestGameInstance(Instance);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Disabled/lifecycle test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Instance = CreateTestGameInstance();
            auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
            DisabledAppOpenedCount = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
            FPostHogConfig Config;
            Config.ProjectToken = TEXT(""); // disabled client
            Config.StorageRoot = Root;
            Config.ClientName = TEXT("disabled-sub");
            Config.bPreloadFeatureFlags = false;
            Config.bCaptureApplicationLifecycleEvents = true;
            Config.BeforeSend = [Counter = DisabledAppOpenedCount](FJsonObject& Event)
            {
                FString Name;
                if (Event.TryGetStringField(TEXT("event"), Name) && Name == TEXT("Application Opened"))
                {
                    Counter->fetch_add(1);
                }
                return true;
            };
            Test->TestTrue(TEXT("disabled setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("disabled setup ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("subsystem reports disabled"), Sub->IsDisabled());
            Test->TestEqual(TEXT("disabled client does not emit Application Opened"), DisabledAppOpenedCount->load(), 0);
            Sub->Shutdown();
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            EnabledAppOpenedCount = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_lifecycle");
            Config.Host = TEXT("http://127.0.0.1:9");
            Config.StorageRoot = Root;
            Config.ClientName = TEXT("lifecycle-on");
            Config.bPreloadFeatureFlags = false;
            Config.bCaptureApplicationLifecycleEvents = true;
            Config.ShutdownTimeoutMilliseconds = 500;
            Config.BeforeSend = [Counter = EnabledAppOpenedCount](FJsonObject& Event)
            {
                FString Name;
                if (Event.TryGetStringField(TEXT("event"), Name) && Name == TEXT("Application Opened"))
                {
                    Counter->fetch_add(1);
                }
                return true;
            };
            Test->TestTrue(TEXT("lifecycle-on setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("lifecycle-on ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            Test->TestFalse(TEXT("enabled client not disabled"), Sub->IsDisabled());
            Test->TestEqual(TEXT("Application Opened emitted once"), EnabledAppOpenedCount->load(), 1);
            Sub->Shutdown();
            Phase = 4;
            return false;
        }
        if (Phase == 4)
        {
            if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            SuppressedAppOpenedCount = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_lifecycle");
            Config.Host = TEXT("http://127.0.0.1:9");
            Config.StorageRoot = Root;
            Config.ClientName = TEXT("lifecycle-off");
            Config.bPreloadFeatureFlags = false;
            Config.bCaptureApplicationLifecycleEvents = false;
            Config.ShutdownTimeoutMilliseconds = 500;
            Config.BeforeSend = [Counter = SuppressedAppOpenedCount](FJsonObject& Event)
            {
                FString Name;
                if (Event.TryGetStringField(TEXT("event"), Name) && Name == TEXT("Application Opened"))
                {
                    Counter->fetch_add(1);
                }
                return true;
            };
            Test->TestTrue(TEXT("lifecycle-off setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 5;
            return false;
        }
        if (Phase == 5)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("lifecycle-off ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            Test->TestEqual(TEXT("Application Opened suppressed when disabled"), SuppressedAppOpenedCount->load(), 0);
            Sub->Shutdown();
            Phase = 6;
            return false;
        }
        if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("final idle"), Sub->GetState(), EPostHogSubsystemState::Idle);
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    FString Root;
    TSharedPtr<std::atomic<int32>, ESPMode::ThreadSafe> DisabledAppOpenedCount;
    TSharedPtr<std::atomic<int32>, ESPMode::ThreadSafe> EnabledAppOpenedCount;
    TSharedPtr<std::atomic<int32>, ESPMode::ThreadSafe> SuppressedAppOpenedCount;
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};

class FSubsystemWarmCacheFlagsLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemWarmCacheFlagsLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        ClientName = TEXT("warm-cache-") + FGuid::NewGuid().ToString();
        Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FSubsystemWarmCacheFlagsLatent()
    {
        DestroyTestGameInstance(Instance);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (Host.IsEmpty())
        {
            Test->AddInfo(TEXT("Skipping warm-cache flag restore test: POSTHOG_TEST_HOST is not set."));
            return true;
        }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Warm-cache flag restore test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Instance = CreateTestGameInstance();
            auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_warm_flags");
            Config.Host = Host;
            Config.StorageRoot = Root;
            Config.ClientName = ClientName;
            Config.bPreloadFeatureFlags = true;
            Config.ShutdownTimeoutMilliseconds = 1000;
            Test->TestTrue(TEXT("warm-up setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("warm-up ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            if (!Sub->GetFeatureFlags().bAvailable) { return false; }
            Sub->Shutdown();
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            Test->TestEqual(TEXT("warm-up shutdown idle"), Sub->GetState(), EPostHogSubsystemState::Idle);

            // Second setup with bPreloadFeatureFlags = false must restore cached flags from disk
            // and notify listeners without making any network request.
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_warm_flags");
            Config.Host = TEXT("http://127.0.0.1:9"); // unreachable: proves restore came from disk cache
            Config.StorageRoot = Root;
            Config.ClientName = ClientName;
            Config.bPreloadFeatureFlags = false;
            Config.ShutdownTimeoutMilliseconds = 500;
            Config.OnFeatureFlagsChanged = [Counter = BgRestoredCount](FPostHogFeatureFlagsUpdate Update)
            {
                if (Update.Snapshot.bAvailable)
                {
                    Counter->fetch_add(1);
                }
            };
            Test->TestTrue(TEXT("restore setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("restore setup ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("restored flags available via getter"), Sub->GetFeatureFlags().bAvailable);
            Test->TestTrue(TEXT("background flag restore fired"), BgRestoredCount->load() >= 1);
            Sub->Shutdown();
            Phase = 4;
            return false;
        }
        if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    FString Root;
    FString ClientName;
    FString Host;
    TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> BgRestoredCount = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};

class FSubsystemSessionReplayGuardsLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemSessionReplayGuardsLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FSubsystemSessionReplayGuardsLatent()
    {
        DestroyTestGameInstance(Instance);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Session replay guards test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Instance = CreateTestGameInstance();
            auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestNotNull(TEXT("subsystem exists"), Sub);
            if (!Sub) { return true; }

            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_replay_guards");
            Config.Host = Host.IsEmpty() ? TEXT("http://127.0.0.1:9") : Host;
            Config.StorageRoot = Root;
            Config.ClientName = TEXT("replay-guards");
            Config.bPreloadFeatureFlags = !Host.IsEmpty();
            Config.ShutdownTimeoutMilliseconds = 500;
            Test->TestTrue(TEXT("replay guards setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("replay guards ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            if (!Host.IsEmpty() && !Sub->GetFeatureFlags().bAvailable) { return false; }

            auto Client = Sub->GetClient();
            Test->TestTrue(TEXT("client available"), Client.IsValid());
            if (!Client)
            {
                Sub->Shutdown();
                Phase = 3;
                return false;
            }

            if (GUsingNullRHI || !FApp::CanEverRender())
            {
                Sub->StartSessionReplay();
                Test->TestFalse(TEXT("subsystem replay inactive when recorder unavailable"), Sub->IsSessionReplayActive());
                Test->TestFalse(TEXT("client replay inactive when recorder unavailable"), Client->IsSessionReplayActive());
            }

            Sub->OptOut();
            Sub->StartSessionReplay();
            Test->TestFalse(TEXT("subsystem replay inactive while opted out"), Sub->IsSessionReplayActive());
            Test->TestFalse(TEXT("client replay inactive while opted out"), Client->IsSessionReplayActive());

            Sub->OptIn();
            Test->TestFalse(TEXT("subsystem replay remains inactive after opt-in"), Sub->IsSessionReplayActive());
            Test->TestFalse(TEXT("client replay remains inactive after opt-in"), Client->IsSessionReplayActive());

            // Start client replay directly, wait for remote config when a live host is configured,
            // then verify OptOut stops replay so OptIn does not silently resume it.
            Client->StartSessionReplay();
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            auto Client = Sub->GetClient();
            if (!Client)
            {
                Sub->Shutdown();
                Phase = 3;
                return false;
            }
            if (!Host.IsEmpty() && !Client->IsSessionReplayActive()) { return false; }
            if (!Host.IsEmpty())
            {
                Test->TestTrue(TEXT("client replay active before opt-out"), Client->IsSessionReplayActive());
            }

            Sub->OptOut();
            Test->TestFalse(TEXT("subsystem replay stopped by opt-out"), Sub->IsSessionReplayActive());
            Test->TestFalse(TEXT("client replay stopped by opt-out"), Client->IsSessionReplayActive());

            Sub->OptIn();
            Test->TestFalse(TEXT("subsystem replay stays inactive after opt-out/opt-in"), Sub->IsSessionReplayActive());
            Test->TestFalse(TEXT("client replay stays inactive after opt-out/opt-in"), Client->IsSessionReplayActive());

            Sub->Shutdown();
            Phase = 3;
            return false;
        }
        if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("replay guards idle"), Sub->GetState(), EPostHogSubsystemState::Idle);
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    FString Root;
    FString Host;
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemLifecycleTest, "PostHog.Subsystem.Lifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemLifecycleTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemLifecycleLatent(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemCancelDuringInitTest, "PostHog.Subsystem.CancelDuringInit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemCancelDuringInitTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemCancelDuringInitLatent(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemDisabledAndLifecycleTest, "PostHog.Subsystem.DisabledAndLifecycle",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemDisabledAndLifecycleTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemDisabledAndLifecycleLatent(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemWarmCacheFlagsTest, "PostHog.Subsystem.WarmCacheFlagsRestored",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemWarmCacheFlagsTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemWarmCacheFlagsLatent(this));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemSessionReplayGuardsTest, "PostHog.Subsystem.SessionReplayGuards",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemSessionReplayGuardsTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemSessionReplayGuardsLatent(this));
    return true;
}
#endif
