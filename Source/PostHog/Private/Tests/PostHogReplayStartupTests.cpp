#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/OutputDevice.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace
{
// Observe recorder lifecycle without adding test-only state to the public API.
class FReplayStartupLog final : public FOutputDevice
{
public:
    std::atomic<int32> Attempts{0}, Starts{0}, Stops{0};
    virtual bool CanBeUsedOnAnyThread() const override { return true; }
    virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
    virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName& Category) override
    {
        if (Category != TEXT("LogPostHogReplay")) { return; }
        const FString Text(Message);
        if (Text.StartsWith(TEXT("Session replay started:"))) { ++Attempts; ++Starts; }
        else if (Text.StartsWith(TEXT("Session replay disabled:"))) { ++Attempts; }
        else if (Text.StartsWith(TEXT("Session replay stopped:"))) { ++Stops; }
    }
};

bool StartFromBlueprint(UPostHogSubsystem* Subsystem)
{
    struct { bool ReturnValue = false; } Params;
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("StartSessionReplay")), &Params);
    return Params.ReturnValue;
}

class FReplayStartupScenario final : public IAutomationLatentCommand
{
public:
    explicit FReplayStartupScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPreviousAutoInitialize = Settings->bAutoInitialize;
        Settings->bAutoInitialize = false;
        Config.ProjectToken = TEXT("phc_MOCK");
        Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogReplayStartupTests") / FGuid::NewGuid().ToString();
        Config.bCaptureApplicationLifecycleEvents = false;
        // The recorder can start without remote permission, but must not capture the editor.
        Config.bPreloadFeatureFlags = false;
        bCanRender = FApp::CanEverRender() && FSlateApplication::IsInitialized() && FSlateApplication::Get().GetRenderer() != nullptr;
        GLog->AddOutputDevice(&Log);
        Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FReplayStartupScenario()
    {
        if (Instance)
        {
            UWorld* World = Instance->GetWorld();
            Instance->Shutdown();
            if (World) { World->SetGameInstance(nullptr); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
            Instance.Reset();
        }
        GLog->RemoveOutputDevice(&Log);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPreviousAutoInitialize;
    }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("Replay startup scenario timed out")); return true; }
        auto* Subsystem = Instance ? Instance->GetSubsystem<UPostHogSubsystem>() : nullptr;
        switch (Phase)
        {
        case 0:
            if (!Config.Host.StartsWith(TEXT("http://127.0.0.1:"))) { Test->AddError(TEXT("Set POSTHOG_TEST_HOST to the local SDK mock server")); return true; }
            Instance.Reset(NewObject<UGameInstance>(GEngine));
            Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
            Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
            Test->TestFalse(TEXT("not ready"), StartFromBlueprint(Subsystem));
            Test->TestTrue(TEXT("setup replay disabled"), Subsystem->SetupWithConfig(Config, true));
            Test->TestFalse(TEXT("initializing"), StartFromBlueprint(Subsystem));
            ++Phase; return false;
        case 1:
            if (!Subsystem->IsInitialized()) { return false; }
            Test->TestFalse(TEXT("local replay opt-in required"), StartFromBlueprint(Subsystem));
            Test->TestEqual(TEXT("disabled does not create a recorder"), Log.Attempts.load(), 0);
            Subsystem->Shutdown();
            ++Phase; return false;
        case 2:
            if (Subsystem->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Config.Replay.bEnabled = true;
            Config.Replay.bAutoStart = false;
            Test->TestTrue(TEXT("setup manual replay"), Subsystem->SetupWithConfig(Config, true));
            ++Phase; return false;
        case 3:
            if (!Subsystem->IsInitialized()) { return false; }
            Test->TestEqual(TEXT("manual mode does not start during setup"), Log.Attempts.load(), 0);
            Test->TestEqual(TEXT("explicit Blueprint start"), StartFromBlueprint(Subsystem), bCanRender);
            Test->TestEqual(TEXT("repeated Blueprint start"), StartFromBlueprint(Subsystem), bCanRender);
            Test->TestEqual(TEXT("one recorder when rendering is available"), Log.Starts.load(), bCanRender ? 1 : 0);
            Test->TestEqual(TEXT("idempotent start, or retry without a renderer"), Log.Attempts.load(), bCanRender ? 1 : 2);
            Test->TestFalse(TEXT("start does not bypass remote permission"), Subsystem->GetClient()->IsSessionReplayAllowed());
            Subsystem->Shutdown();
            Test->TestFalse(TEXT("shutdown rejects start"), StartFromBlueprint(Subsystem));
            Test->TestEqual(TEXT("shutdown stops the recorder"), Log.Stops.load(), bCanRender ? 1 : 0);
            ++Phase; return false;
        case 4:
            if (Subsystem->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Config.Replay.bAutoStart = true;
            Test->TestTrue(TEXT("setup automatic replay"), Subsystem->SetupWithConfig(Config, true));
            ++Phase; return false;
        case 5:
            if (!Subsystem->IsInitialized()) { return false; }
            Test->TestEqual(TEXT("automatic mode starts during setup"), Log.Attempts.load(), bCanRender ? 2 : 3);
            Test->TestEqual(TEXT("automatic mode creates a second recorder"), Log.Starts.load(), bCanRender ? 2 : 0);
            Subsystem->Shutdown();
            ++Phase; return false;
        case 6:
            if (Subsystem->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Test->TestEqual(TEXT("all recorders stopped"), Log.Stops.load(), bCanRender ? 2 : 0);
            IFileManager::Get().DeleteDirectory(*Config.StorageRoot, false, true);
            return true;
        }
        return false;
    }
private:
    FAutomationTestBase* Test;
    FPostHogConfig Config;
    TStrongObjectPtr<UGameInstance> Instance;
    FReplayStartupLog Log;
    bool bPreviousAutoInitialize = false;
    bool bCanRender = false;
    int32 Phase = 0;
    double Deadline = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogReplayStartupTest, "PostHog.ReplayStartup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogReplayStartupTest::RunTest(const FString&)
{
    const auto* Function = UPostHogSubsystem::StaticClass()->FindFunctionByName(TEXT("StartSessionReplay"));
    TestTrue(TEXT("Blueprint callable, not pure"), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FReplayStartupScenario>(this));
    return true;
}
#endif
