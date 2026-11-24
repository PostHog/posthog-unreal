#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Analytics.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
class FAnalyticsLatent final : public IAutomationLatentCommand
{
public:
    explicit FAnalyticsLatent(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPrevAuto = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogAnalyticsTests") / FGuid::NewGuid().ToString();
        Deadline = FPlatformTime::Seconds() + 15.0;
    }

    ~FAnalyticsLatent()
    {
        if (Instance.IsValid())
        {
            Instance->Shutdown();
            if (GEngine) { GEngine->DestroyWorldContext(Instance->GetWorld()); }
            Instance.Reset();
        }
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPrevAuto;
        IFileManager::Get().DeleteDirectory(*Root, false, true);
    }

    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline)
        {
            Test->AddError(TEXT("Analytics test timed out"));
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

            auto* Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestFalse(TEXT("uninitialized cannot bind"), Subsystem->BindAnalyticsProvider());

            FPostHogConfig Config;
            Config.ProjectToken = TEXT("phc_test_analytics");
            Config.Host = TEXT("http://127.0.0.1:9");
            Config.StorageRoot = Root;
            Config.SessionMode = EPostHogSessionMode::Manual;
            Config.bPreloadFeatureFlags = false;
            Config.bCaptureApplicationLifecycleEvents = false;
            Config.ShutdownTimeoutMilliseconds = 500;
            Test->TestTrue(TEXT("setup started"), Subsystem->SetupWithConfig(MoveTemp(Config), true, true));
            Phase = 1;
            return false;
        }
        auto* Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
        if (Phase == 1)
        {
            if (Subsystem->GetState() == EPostHogSubsystemState::Initializing) { return false; }
            Test->TestEqual(TEXT("ready"), Subsystem->GetState(), EPostHogSubsystemState::Ready);
            Test->TestTrue(TEXT("bind analytics provider"), Subsystem->BindAnalyticsProvider());

            auto Provider = FPostHogAnalyticsProvider::GetBoundProvider();
            Test->TestTrue(TEXT("bound provider resolved"), Provider.IsValid());
            if (Provider)
            {
                Test->TestFalse(TEXT("SetSessionID unsupported"), Provider->SetSessionID(TEXT("custom")));
                Test->TestTrue(TEXT("StartSession succeeds"), Provider->StartSession());
                Test->TestFalse(TEXT("session id created"), Provider->GetSessionID().IsEmpty());

                TArray<FAnalyticsEventAttribute> Defaults;
                Defaults.Emplace(TEXT("env"), TEXT("test"));
                Provider->SetDefaultEventAttributes(MoveTemp(Defaults));
                Test->TestEqual(TEXT("default attribute count"), Provider->GetDefaultEventAttributeCount(), 1);

                Provider->SetUserID(TEXT("analytics-user"));
                Test->TestEqual(TEXT("user id updated"), Provider->GetUserID(), FString(TEXT("analytics-user")));
                Provider->RecordEvent(TEXT("analytics_event"), {FAnalyticsEventAttribute(TEXT("k"), TEXT("v"))});
                Provider->EndSession();
            }

            Subsystem->UnbindAnalyticsProvider();
            Test->TestFalse(TEXT("unbound provider cleared"), FPostHogAnalyticsProvider::GetBoundProvider().IsValid());
            Subsystem->Shutdown();
            Phase = 2;
            return false;
        }
        if (Subsystem->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
        Test->TestEqual(TEXT("idle after shutdown"), Subsystem->GetState(), EPostHogSubsystemState::Idle);
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
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogAnalyticsProviderTest, "PostHog.Analytics.Provider",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogAnalyticsProviderTest::RunTest(const FString&)
{
    ADD_LATENT_AUTOMATION_COMMAND(FAnalyticsLatent(this));
    return true;
}
#endif
