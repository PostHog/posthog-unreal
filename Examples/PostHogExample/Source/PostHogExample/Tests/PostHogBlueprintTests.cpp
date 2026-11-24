#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogBlueprintProbe.h"
#include "PostHogBlueprintLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>
#if !PLATFORM_EXCEPTIONS_DISABLED
#include <stdexcept>
#endif

namespace
{
class FBlueprintDelegateProbeLatent final : public IAutomationLatentCommand
{
public:
    explicit FBlueprintDelegateProbeLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogBlueprintTests") / FGuid::NewGuid().ToString();
        ClientName = TEXT("bp-probe-") + FGuid::NewGuid().ToString();
        Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FBlueprintDelegateProbeLatent()
    {
        if (Instance.IsValid())
        {
            Instance->Shutdown();
            if (GEngine) { GEngine->DestroyWorldContext(Instance->GetWorld()); }
            Instance.Reset();
        }
        Probe.Reset();
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Blueprint delegate probe test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Instance.Reset(NewObject<UGameInstance>(GEngine));
            FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
            Context.OwningGameInstance = Instance.Get();
            UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
            Context.SetCurrentWorld(World);
            Instance->Init();

            auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestNotNull(TEXT("subsystem exists"), Sub);
            if (!Sub) { return true; }

            Probe.Reset(NewObject<UPostHogBlueprintProbe>());
            Sub->OnFeatureFlagsChanged.AddDynamic(Probe.Get(), &UPostHogBlueprintProbe::OnFlagsChanged);
            Test->TestTrue(TEXT("probe bound to OnFeatureFlagsChanged"),
                Sub->OnFeatureFlagsChanged.IsAlreadyBound(Probe.Get(), &UPostHogBlueprintProbe::OnFlagsChanged));

            if (Host.IsEmpty())
            {
                // Exercise reflected dynamic delegate dispatch directly when offline.
                FPostHogFeatureFlagsUpdate Synthetic;
                Synthetic.Snapshot.bAvailable = true;
                Sub->OnFeatureFlagsChanged.Broadcast(Synthetic);
                Test->TestEqual(TEXT("probe received synthetic broadcast"), Probe->Completions, 1);
                Test->TestTrue(TEXT("probe ran on game thread"), Probe->bGameThread);
                Test->TestTrue(TEXT("probe received snapshot"), Probe->LastUpdate.Snapshot.bAvailable);
                return true;
            }

            // Phase 0 -> 1: load flags from mock server to populate disk cache, verifying live OnFeatureFlagsChanged.
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_bp_probe");
            Config.Host = Host;
            Config.StorageRoot = Root;
            Config.ClientName = ClientName;
            Config.bPreloadFeatureFlags = true;
            Config.ShutdownTimeoutMilliseconds = 1000;
            Test->TestTrue(TEXT("preload setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            if (Probe->Completions < 1) { return false; }
            Test->TestTrue(TEXT("live flag broadcast ran on game thread"), Probe->bGameThread);
            Test->TestTrue(TEXT("live flag broadcast snapshot available"), Probe->LastUpdate.Snapshot.bAvailable);
            Sub->Shutdown();
            Phase = 2;
            return false;
        }
        if (Phase == 2)
        {
            if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            // Phase 2 -> 3: warm-cache restore with bPreloadFeatureFlags = false must fire
            // UPostHogSubsystem::OnFeatureFlagsChanged on the GameThread after setup becomes Ready!
            Probe->Completions = 0;
            Probe->LastUpdate = FPostHogFeatureFlagsUpdate{};
            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_bp_probe");
            Config.Host = TEXT("http://127.0.0.1:9");
            Config.StorageRoot = Root;
            Config.ClientName = ClientName;
            Config.bPreloadFeatureFlags = false;
            Config.ShutdownTimeoutMilliseconds = 500;
            Test->TestTrue(TEXT("warm-restore setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 3;
            return false;
        }
        if (Phase == 3)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("warm-restore ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            // Buffered restored update is drained synchronously right after OnSetupCompleted(true),
            // or arrives on the GameThread tick immediately after.
            if (Probe->Completions < 1) { return false; }
            Test->TestTrue(TEXT("warm-restore delegate ran on game thread"), Probe->bGameThread);
            Test->TestTrue(TEXT("warm-restore delegate snapshot available"), Probe->LastUpdate.Snapshot.bAvailable);
            Test->TestTrue(TEXT("getters reflect restored flags"), Sub->GetFeatureFlags().bAvailable);
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
    TStrongObjectPtr<UPostHogBlueprintProbe> Probe;
    FString Root;
    FString ClientName;
    FString Host;
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};

#if !PLATFORM_EXCEPTIONS_DISABLED
class FSubsystemFlagCallbackExceptionLatent final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemFlagCallbackExceptionLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogBlueprintTests") / FGuid::NewGuid().ToString();
        Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Deadline = FPlatformTime::Seconds() + 20.0;
    }

    ~FSubsystemFlagCallbackExceptionLatent()
    {
        if (Instance.IsValid())
        {
            Instance->Shutdown();
            if (GEngine) { GEngine->DestroyWorldContext(Instance->GetWorld()); }
            Instance.Reset();
        }
        Probe.Reset();
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (Host.IsEmpty())
        {
            Test->AddInfo(TEXT("Skipping subsystem flag callback exception test: POSTHOG_TEST_HOST is not set."));
            return true;
        }
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(FString::Printf(TEXT("Subsystem flag callback exception test timed out in phase %d"), Phase));
            return true;
        }
        if (Phase == 0)
        {
            Test->AddExpectedMessage(TEXT("OnFeatureFlagsChanged threw std::exception."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

            Instance.Reset(NewObject<UGameInstance>(GEngine));
            FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
            Context.OwningGameInstance = Instance.Get();
            UWorld* World = UWorld::CreateWorld(EWorldType::Game, false);
            Context.SetCurrentWorld(World);
            Instance->Init();

            auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestNotNull(TEXT("subsystem exists"), Sub);
            if (!Sub) { return true; }

            Probe.Reset(NewObject<UPostHogBlueprintProbe>());
            Sub->OnFeatureFlagsChanged.AddDynamic(Probe.Get(), &UPostHogBlueprintProbe::OnFlagsChanged);

            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_sub_flag_throw");
            Config.Host = Host;
            Config.StorageRoot = Root;
            Config.ClientName = TEXT("flag-throw");
            Config.bPreloadFeatureFlags = true;
            Config.ShutdownTimeoutMilliseconds = 500;
            Config.OnFeatureFlagsChanged = [Threw = bThrew](FPostHogFeatureFlagsUpdate)
            {
                if (!Threw->exchange(true))
                {
                    throw std::runtime_error("secret_subsystem_flag_exception");
                }
            };
            Test->TestTrue(TEXT("setup accepted"), Sub->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Sub = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Sub->GetState() == EPostHogSubsystemState::Initializing || Probe->Completions < 1) { return false; }
            Test->TestEqual(TEXT("subsystem ready"), Sub->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("throwing user callback ran"), bThrew->load());
            Test->TestTrue(TEXT("subsystem OnFeatureFlagsChanged fired on game thread"), Probe->Completions >= 1 && Probe->bGameThread);
            Test->TestTrue(TEXT("subsystem OnFeatureFlagsChanged received snapshot"), Probe->LastUpdate.Snapshot.bAvailable);
            Sub->Shutdown();
            Phase = 2;
            return false;
        }
        if (Sub->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("subsystem idle"), Sub->GetState(), EPostHogSubsystemState::Idle);
        return true;
    }

private:
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    TStrongObjectPtr<UPostHogBlueprintProbe> Probe;
    FString Root;
    FString Host;
    TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bThrew = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    bool bPrevAuto = true;
    double Deadline = 0;
    int32 Phase = 0;
};
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogBlueprintReflectionTest, "PostHog.Blueprint.Reflection",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogBlueprintReflectionTest::RunTest(const FString&)
{
    const TCHAR* ExpectedFunctions[] = {
        TEXT("Setup"), TEXT("Shutdown"), TEXT("IsInitialized"), TEXT("IsDisabled"), TEXT("GetState"),
        TEXT("Capture"), TEXT("Identify"), TEXT("Reset"), TEXT("OptIn"), TEXT("OptOut"),
        TEXT("GetFeatureFlag"), TEXT("GetFeatureFlags"), TEXT("ReloadFeatureFlags"),
        TEXT("StartSessionReplay"), TEXT("StopSessionReplay"), TEXT("IsSessionReplayActive"),
        TEXT("BindAnalyticsProvider"), TEXT("UnbindAnalyticsProvider")
    };
    for (const TCHAR* Name : ExpectedFunctions)
    {
        TestNotNull(FString::Printf(TEXT("UPostHogSubsystem::%s reflected"), Name),
            UPostHogSubsystem::StaticClass()->FindFunctionByName(Name));
    }

    FPostHogProperties Props;
    TestTrue(TEXT("set string"), UPostHogBlueprintLibrary::SetProperty(Props, TEXT("a"), UPostHogBlueprintLibrary::MakeStringValue(TEXT("x"))));
    TestTrue(TEXT("set bool"), UPostHogBlueprintLibrary::SetProperty(Props, TEXT("b"), UPostHogBlueprintLibrary::MakeBooleanValue(true)));
    TestTrue(TEXT("set number"), UPostHogBlueprintLibrary::SetProperty(Props, TEXT("c"), UPostHogBlueprintLibrary::MakeNumberValue(3.5)));
    TestFalse(TEXT("invalid json rejected"), UPostHogBlueprintLibrary::ParseProperties(TEXT("[]"), Props));
    TestEqual(TEXT("failed parse clears output"), UPostHogBlueprintLibrary::PropertiesToJson(Props), FString(TEXT("{}")));

    bool BoolOut = false;
    FString StrOut;
    TestTrue(TEXT("try get bool"), UPostHogBlueprintLibrary::TryGetBoolean(UPostHogBlueprintLibrary::MakeBooleanValue(true), BoolOut) && BoolOut);
    TestFalse(TEXT("no bool coercion"), UPostHogBlueprintLibrary::TryGetBoolean(UPostHogBlueprintLibrary::MakeStringValue(TEXT("true")), BoolOut));
    TestTrue(TEXT("try get string"), UPostHogBlueprintLibrary::TryGetString(UPostHogBlueprintLibrary::MakeStringValue(TEXT("v1")), StrOut) && StrOut == TEXT("v1"));
    TestEqual(TEXT("null value json"), UPostHogBlueprintLibrary::ValueToJson(UPostHogBlueprintLibrary::MakeNullValue()), FString(TEXT("null")));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogBlueprintDelegateProbeTest, "PostHog.Blueprint.DelegateProbe",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogBlueprintDelegateProbeTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FBlueprintDelegateProbeLatent(this));
    return true;
}

#if !PLATFORM_EXCEPTIONS_DISABLED
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemFlagCallbackExceptionTest, "PostHog.Subsystem.FlagCallbackException",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogSubsystemFlagCallbackExceptionTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FSubsystemFlagCallbackExceptionLatent(this));
    return true;
}
#endif
#endif
