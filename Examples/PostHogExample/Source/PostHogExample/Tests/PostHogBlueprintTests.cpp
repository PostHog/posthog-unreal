#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogBlueprintProbe.h"
#include "PostHogBlueprintLibrary.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
EPostHogResult Invoke(UPostHogSubsystem* Subsystem, const TCHAR* Name)
{
    struct { EPostHogResult ReturnValue = EPostHogResult::Closed; } Params;
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(Name), &Params); return Params.ReturnValue;
}
EPostHogResult Capture(UPostHogSubsystem* Subsystem, const FString& Event, const FPostHogProperties& Properties = {})
{
    struct { FString Event; FPostHogProperties Properties; EPostHogResult ReturnValue; } Params{Event, Properties, EPostHogResult::Closed};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("Capture")), &Params); return Params.ReturnValue;
}
EPostHogResult Identify(UPostHogSubsystem* Subsystem, const FString& Id)
{
    struct { FString DistinctId; FPostHogProperties Set; FPostHogProperties SetOnce; EPostHogResult ReturnValue; } Params{Id, {}, {}, EPostHogResult::Closed};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("Identify")), &Params); return Params.ReturnValue;
}
FPostHogFeatureFlagResult Flag(UPostHogSubsystem* Subsystem, const FString& Key)
{
    struct { FString Key; EPostHogFlagReporting Reporting; FPostHogFeatureFlagResult ReturnValue; } Params{Key, EPostHogFlagReporting::Silent, {}};
    Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("GetFeatureFlag")), &Params); return Params.ReturnValue;
}
class FBlueprintScenario final : public IAutomationLatentCommand
{
public:
    explicit FBlueprintScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>(); PreviousAuto = Settings->bAutoInitialize; Settings->bAutoInitialize = false;
        Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogBlueprintTests") / FGuid::NewGuid().ToString();
        Config.bPreloadFeatureFlags = true; Config.Replay.MaxFramesInFlight = 1; Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FBlueprintScenario() { Destroy(); GetMutableDefault<UPostHogSettings>()->bAutoInitialize = PreviousAuto; }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("Blueprint scenario timed out")); return true; }
        auto* Subsystem = Instance ? Instance->GetSubsystem<UPostHogSubsystem>() : nullptr;
        switch (Phase)
        {
        case 0:
            if (Config.Host.IsEmpty()) { Test->AddError(TEXT("Run with the native test driver")); return true; }
            Instance.Reset(NewObject<UGameInstance>(GEngine)); Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
            Subsystem = Instance->GetSubsystem<UPostHogSubsystem>(); Probe.Reset(NewObject<UPostHogBlueprintProbe>());
            Subsystem->OnFeatureFlagsChanged.AddDynamic(Probe.Get(), &UPostHogBlueprintProbe::OnUpdate);
            Test->TestEqual(TEXT("not-ready capture"), Capture(Subsystem, TEXT("not-ready")), EPostHogResult::Closed);
            Test->TestEqual(TEXT("not-ready reload"), Invoke(Subsystem, TEXT("ReloadFeatureFlags")), EPostHogResult::Closed);
            Test->TestTrue(TEXT("setup"), Subsystem->SetupWithConfig(Config, true)); ++Phase; return false;
        case 1:
        {
            if (!Subsystem->IsInitialized()) { return false; }
            Client = Subsystem->GetClient(); Client->SetAppInBackground(false);
            FPostHogProperties Properties;
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("string"), UPostHogBlueprintLibrary::MakeStringValue(TEXT("00123 玩家 🚀")));
            UPostHogBlueprintLibrary::SetProperty(Properties, TEXT("boolean"), UPostHogBlueprintLibrary::MakeBooleanValue(false));
            Test->TestEqual(TEXT("reflected capture"), Capture(Subsystem, TEXT("blueprint_anonymous"), Properties), EPostHogResult::Accepted);
            Test->TestEqual(TEXT("identify invocation"), Identify(Subsystem, TEXT("blueprint-user")), EPostHogResult::Ok);
            Test->TestEqual(TEXT("duplicate invokes core without synthetic rejection"), Identify(Subsystem, TEXT("blueprint-user")), EPostHogResult::Ok);
            Test->TestEqual(TEXT("identified capture"), Capture(Subsystem, TEXT("blueprint_identified")), EPostHogResult::Accepted);
            Drain = Client->FlushAsync(); ++Phase; return false;
        }
        case 2:
        {
            if (!Drain.IsReady() || Probe->Notifications == 0) { return false; }
            Test->TestTrue(TEXT("SDK updates dispatched on game thread"), Probe->bGameThread);
            const auto KnownFalse = Flag(Subsystem, TEXT("disabled-flag"));
            Test->TestTrue(TEXT("known false result"), KnownFalse.Status == EPostHogResult::Ok && KnownFalse.bFound && !KnownFalse.bEnabled);
            Test->TestFalse(TEXT("missing distinct from false"), Flag(Subsystem, TEXT("missing")).bFound);
            BaselineNotifications = Probe->Notifications;
            Test->TestEqual(TEXT("refresh invocation"), Invoke(Subsystem, TEXT("ReloadFeatureFlags")), EPostHogResult::Ok);
            Drain = Client->FlushAsync(); ++Phase; return false;
        }
        case 3:
        {
            if (!Drain.IsReady() || Probe->Notifications <= BaselineNotifications) { return false; }
            Test->TestEqual(TEXT("manual replay request"), Invoke(Subsystem, TEXT("StartSessionReplay")), EPostHogResult::Ok);
            struct { int32 Width; int32 Height; int32 RowStrideBytes; UPostHogReplayFrame* Frame; EPostHogResult ReturnValue; } Params{32, 24, 128, nullptr, EPostHogResult::Closed};
            Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("BeginReplayFrame")), &Params);
            Test->TestEqual(TEXT("reflected owned acquisition"), Params.ReturnValue, EPostHogResult::Ok);
            Frame.Reset(Params.Frame);
            if (!Frame) { return true; }
            struct { TArray<uint8> Pixels; EPostHogResult ReturnValue; } SubmitParams;
            SubmitParams.Pixels.SetNumZeroed(128 * 24); // Deliberately fully masked synthetic image.
            Frame->ProcessEvent(Frame->FindFunctionChecked(TEXT("Submit")), &SubmitParams);
            Test->TestEqual(TEXT("reflected prepared pixels"), SubmitParams.ReturnValue, EPostHogResult::Accepted);
            Test->TestEqual(TEXT("lease consumed"), Frame->Abandon(), EPostHogResult::Closed);
            Frame.Reset(); Drain = Client->FlushAsync(); ++Phase; return false;
        }
        case 4:
            if (!Drain.IsReady()) { return false; }
            Test->TestEqual(TEXT("opt-out invocation"), Invoke(Subsystem, TEXT("OptOut")), EPostHogResult::Ok);
            Test->TestEqual(TEXT("opted-out capture"), Capture(Subsystem, TEXT("blocked-blueprint")), EPostHogResult::OptedOut);
            BaselineNotifications = Probe->Notifications; Subsystem->Shutdown(); ++Phase; return false;
        case 5:
            if (Subsystem->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Test->TestEqual(TEXT("shutdown cancels notifications"), Probe->Notifications, BaselineNotifications);
            Test->TestTrue(TEXT("restart same owner namespace"), Subsystem->SetupWithConfig(Config, true)); ++Phase; return false;
        case 6:
            if (!Subsystem->IsInitialized()) { return false; }
            Client = Subsystem->GetClient(); Destroy(); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Drain = Client->ShutdownAsync(); ++Phase; return false;
        case 7:
            if (!Drain.IsReady()) { return false; }
            Test->TestTrue(TEXT("retained native lifetime joins after UObject destruction"), Drain.Get().bJoined);
            Client.Reset(); return true;
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
    FPostHogConfig Config;
    bool PreviousAuto = false;
    int32 Phase = 0, BaselineNotifications = 0;
    double Deadline;
    TStrongObjectPtr<UGameInstance> Instance;
    TStrongObjectPtr<UPostHogBlueprintProbe> Probe;
    TStrongObjectPtr<UPostHogReplayFrame> Frame;
    FPostHogClient::FPtr Client;
    TSharedFuture<FPostHogDrainResult> Drain;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogBlueprintTest, "PostHog.Blueprint", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogBlueprintTest::RunTest(const FString&)
{ FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FBlueprintScenario>(this)); return true; }
#endif
