#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogBlueprintProbe.h"
#include "PostHogBlueprintLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
// Exercise the reflected entry points used by Blueprint, not just direct C++ calls.
EPostHogCaptureResult Capture(UPostHogSubsystem* Subsystem, const FString& Event, const FPostHogProperties& Properties = {})
{
    struct { FString Event; FPostHogProperties Properties; EPostHogCaptureResult ReturnValue; }
        Params{Event, Properties, EPostHogCaptureResult::Closed};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("Capture")), &Params);
    return Params.ReturnValue;
}
bool Identify(UPostHogSubsystem* Subsystem, const FString& Id, const FPostHogProperties& Set = {}, const FPostHogProperties& SetOnce = {})
{
    struct { FString DistinctId; FPostHogProperties Set; FPostHogProperties SetOnce; bool ReturnValue; }
        Params{Id, Set, SetOnce, false};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("Identify")), &Params);
    return Params.ReturnValue;
}
FPostHogFeatureFlagResult GetFlag(UPostHogSubsystem* Subsystem, const FString& Key, bool bSendEvent = true)
{
    struct { FString Key; bool bSendEvent; FPostHogFeatureFlagResult ReturnValue; } Params{Key, bSendEvent, {}};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("GetFeatureFlag")), &Params);
    return Params.ReturnValue;
}
bool Reload(UPostHogSubsystem* Subsystem)
{
    struct { bool ReturnValue = false; } Params;
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("ReloadFeatureFlags")), &Params);
    return Params.ReturnValue;
}

class FBlueprintScenario final : public IAutomationLatentCommand
{
public:
    explicit FBlueprintScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPreviousAuto = Settings->bAutoInitialize; Settings->bAutoInitialize = false;
        Root = FPaths::ProjectSavedDir() / TEXT("PostHogBlueprintTests") / FGuid::NewGuid().ToString();
        Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = Root; Config.bPreloadFeatureFlags = false; Config.bCaptureApplicationLifecycleEvents = false;
        Config.FlushAt = 100; Config.FlushIntervalMilliseconds = 60000;
        Config.Context.SetString(TEXT("test_suite"), TEXT("blueprint"));
        Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FBlueprintScenario()
    {
        Destroy();
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPreviousAuto;
    }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("Blueprint scenario timed out")); return true; }
        auto* Subsystem = Instance ? Instance->GetSubsystem<UPostHogSubsystem>() : nullptr;
        switch (Phase)
        {
        case 0:
            if (Config.Host.IsEmpty()) { Test->AddError(TEXT("Run with Tests/native_integration.py")); return true; }
            Instance.Reset(NewObject<UGameInstance>(GEngine));
            Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
            Subsystem = Instance->GetSubsystem<UPostHogSubsystem>();
            Probe.Reset(NewObject<UPostHogBlueprintProbe>()); Probe->Subsystem = Subsystem;
            Subsystem->OnFeatureFlagsReloaded.AddDynamic(Probe.Get(), &UPostHogBlueprintProbe::OnReload);
            Test->TestEqual(TEXT("not-ready capture"), Capture(Subsystem, TEXT("blueprint_not_ready")), EPostHogCaptureResult::Closed);
            Test->TestFalse(TEXT("not-ready identify"), Identify(Subsystem, TEXT("ignored")));
            Test->TestFalse(TEXT("not-ready getter"), GetFlag(Subsystem, TEXT("bool-value")).bFound);
            Test->TestFalse(TEXT("not-ready reload"), Reload(Subsystem));
            Test->TestTrue(TEXT("setup"), Subsystem->SetupWithConfig(Config, true));
            Test->TestFalse(TEXT("initializing reload"), Reload(Subsystem));
            ++Phase; return false;
        case 1:
        {
            if (!Subsystem->IsInitialized()) { return false; }
            FPostHogProperties Properties, Set, SetOnce;
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("string"), UPostHogBlueprintLibrary::MakeStringValue(TEXT("00123 玩家 🚀")));
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("number"), UPostHogBlueprintLibrary::MakeNumberValue(42));
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("boolean"), UPostHogBlueprintLibrary::MakeBooleanValue(false));
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("array"), UPostHogBlueprintLibrary::MakeArrayValue({UPostHogBlueprintLibrary::MakeNullValue(), UPostHogBlueprintLibrary::MakeObjectValue(Properties)}));
            Test->TestEqual(TEXT("capture"), Capture(Subsystem, TEXT("blueprint_anonymous"), Properties), EPostHogCaptureResult::Accepted);
            Test->TestTrue(TEXT("parse set"), UPostHogBlueprintLibrary::ParseProperties(TEXT("{\"name\":\"Blueprint player\"}"), Set));
            UPostHogBlueprintLibrary::SetProperty(SetOnce, TEXT("origin"), UPostHogBlueprintLibrary::MakeStringValue(TEXT("blueprint")));
            Test->TestTrue(TEXT("identify"), Identify(Subsystem, TEXT("blueprint-user"), Set, SetOnce));
            Test->TestFalse(TEXT("duplicate identify"), Identify(Subsystem, TEXT("blueprint-user"), Set, SetOnce));
            Test->TestEqual(TEXT("identified capture"), Capture(Subsystem, TEXT("blueprint_identified")), EPostHogCaptureResult::Accepted);
            Test->TestEqual(TEXT("invalid capture"), Capture(Subsystem, TEXT("")), EPostHogCaptureResult::Invalid);
            Test->TestFalse(TEXT("invalid properties JSON"), UPostHogBlueprintLibrary::ParseProperties(TEXT("[]"), Set));
            Test->TestEqual(TEXT("failed parse clears output"), UPostHogBlueprintLibrary::PropertiesToJson(Set), FString(TEXT("{}")));
            Flushed = Subsystem->GetClient()->FlushAsync();
            Probe->bReloadAgain = true;
            Test->TestTrue(TEXT("start reload"), Reload(Subsystem));
            Test->TestTrue(TEXT("coalesce reload"), Reload(Subsystem));
            Test->TestEqual(TEXT("completion is deferred"), Probe->Completions, 0);
            ++Phase; return false;
        }
        case 2:
        {
            if (Probe->Completions < 2 || !Flushed.IsReady()) { return false; }
            Test->TestTrue(TEXT("capture flush"), Flushed.Get().bCompleted);
            Test->TestEqual(TEXT("one completion per cycle including reentrant reload"), Probe->Completions, 2);
            Test->TestTrue(TEXT("game-thread delegate"), Probe->bGameThread);
            Test->TestEqual(TEXT("successful reload"), Probe->LastResult.Status, EPostHogFlagReloadStatus::Success);
            bool Boolean = true; FString String;
            auto Flag = GetFlag(Subsystem, TEXT("disabled-flag"));
            Test->TestTrue(TEXT("known false"), Flag.bFound && UPostHogBlueprintLibrary::TryGetBoolean(Flag.Value, Boolean) && !Boolean);
            GetFlag(Subsystem, TEXT("disabled-flag")); // The native client deduplicates exposures.
            Test->TestFalse(TEXT("missing"), GetFlag(Subsystem, TEXT("missing")).bFound);
            Flag = GetFlag(Subsystem, TEXT("string-value"), false);
            Test->TestTrue(TEXT("variant"), Flag.bFound && UPostHogBlueprintLibrary::TryGetString(Flag.Value, String) && String == TEXT("test"));
            Test->TestFalse(TEXT("no boolean coercion"), UPostHogBlueprintLibrary::TryGetBoolean(Flag.Value, Boolean));
            Test->TestFalse(TEXT("no string coercion"), UPostHogBlueprintLibrary::TryGetString(UPostHogBlueprintLibrary::MakeBooleanValue(true), String));
            Test->TestEqual(TEXT("null value"), UPostHogBlueprintLibrary::ValueToJson(UPostHogBlueprintLibrary::MakeNullValue()), FString(TEXT("null")));
            Flushed = Subsystem->GetClient()->FlushAsync();
            Reload(Subsystem);
            // Test-only bounded wait: invalidate after native completion but before the next ticker notification.
            auto NativeReload = Subsystem->GetClient()->ReloadFeatureFlagsAsync();
            if (!NativeReload.WaitFor(FTimespan::FromSeconds(5))) { Test->AddError(TEXT("native reload timed out")); return true; }
            Transition = Subsystem->GetClient()->ResetAsync();
            ++Phase; return false;
        }
        case 3:
            if (Probe->Completions < 3 || !Transition.IsReady() || !Flushed.IsReady()) { return false; }
            Test->TestTrue(TEXT("exposure flush"), Flushed.Get().bCompleted);
            Test->TestEqual(TEXT("deferred old-context snapshot rejected"), Probe->LastResult.Status, EPostHogFlagReloadStatus::Stale);
            Test->TestTrue(TEXT("stale snapshot cleared"), Probe->LastResult.Flags.IsEmpty());
            Transition = Subsystem->GetClient()->OptOutAsync();
            Test->TestEqual(TEXT("opted-out capture"), Capture(Subsystem, TEXT("blueprint_opted_out")), EPostHogCaptureResult::Disabled);
            Test->TestTrue(TEXT("opted-out reload accepted for completion"), Reload(Subsystem));
            ++Phase; return false;
        case 4:
            if (Probe->Completions < 4 || !Transition.IsReady()) { return false; }
            Test->TestEqual(TEXT("disabled completion"), Probe->LastResult.Status, EPostHogFlagReloadStatus::Disabled);
            Transition = Subsystem->GetClient()->OptInAsync(); ++Phase; return false;
        case 5:
            if (!Transition.IsReady()) { return false; }
            Test->TestTrue(TEXT("opt in"), Transition.Get());
            Reload(Subsystem); Subsystem->Shutdown();
            Test->TestFalse(TEXT("shutdown rejects reload"), Reload(Subsystem));
            Test->TestFalse(TEXT("shutdown getter is missing"), GetFlag(Subsystem, TEXT("string-value")).bFound);
            ++Phase; return false;
        case 6:
            if (Subsystem->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Test->TestEqual(TEXT("shutdown cancels notification"), Probe->Completions, 4);
            Test->TestTrue(TEXT("restart"), Subsystem->SetupWithConfig(Config, true)); ++Phase; return false;
        case 7:
            if (!Subsystem->IsInitialized()) { return false; }
            Reload(Subsystem);
            Client = Subsystem->GetClient(); Destroy();
            CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Test->TestFalse(TEXT("subsystem weak reference expires"), Probe->Subsystem.IsValid());
            Transition = Client->ShutdownAsync(); ++Phase; return false;
        case 8:
            if (!Transition.IsReady()) { return false; }
            Test->TestEqual(TEXT("owner destruction cancels notification"), Probe->Completions, 4);
            Client.Reset(); IFileManager::Get().DeleteDirectory(*Root, false, true);
            return true;
        }
        return false;
    }
private:
    void Destroy()
    {
        if (!Instance) { return; }
        UWorld* World = Instance->GetWorld(); Instance->Shutdown();
        if (World) { World->SetGameInstance(nullptr); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        Instance.Reset();
    }
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> Instance;
    TStrongObjectPtr<UPostHogBlueprintProbe> Probe;
    FPostHogConfig Config;
    FPostHogClient::FPtr Client;
    TSharedFuture<bool> Transition;
    TSharedFuture<FPostHogDeliveryResult> Flushed;
    FString Root;
    bool bPreviousAuto = false;
    int32 Phase = 0;
    double Deadline = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogBlueprintTest, "PostHog.Blueprint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogBlueprintTest::RunTest(const FString& Parameters)
{
    for (const FName Name : {FName(TEXT("Capture")), FName(TEXT("Identify")), FName(TEXT("GetFeatureFlag")), FName(TEXT("ReloadFeatureFlags"))})
    {
        const auto* Function = UPostHogSubsystem::StaticClass()->FindFunctionByName(Name);
        TestTrue(*Name.ToString(), Function && Function->HasAnyFunctionFlags(FUNC_BlueprintCallable) && !Function->HasAnyFunctionFlags(FUNC_BlueprintPure));
    }
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FBlueprintScenario>(this));
    return true;
}
#endif
