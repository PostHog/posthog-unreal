#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogBlueprintProbe.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace
{
enum class ELifecycleCase : uint8
{
    Foreground, InitialBackground, Disabled, OptedOut, Dropped,
    StartupShutdown, StartupDestroy, BackgroundShutdown, ForegroundDestroy, Count
};
class FLifecycleScenario final : public IAutomationLatentCommand
{
public:
    explicit FLifecycleScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>(); PreviousAuto = Settings->bAutoInitialize; Settings->bAutoInitialize = false;
        Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Deadline = FPlatformTime::Seconds() + 60;
        Test->TestTrue(TEXT("native lifecycle capture defaults on"), FPostHogConfig().bCaptureApplicationLifecycleEvents);
        FPostHogOptions Options;
        Test->TestTrue(TEXT("Blueprint lifecycle capture defaults on"), Options.bCaptureApplicationLifecycleEvents);
        Options.bCaptureApplicationLifecycleEvents = false;
        Test->TestFalse(TEXT("Blueprint option reaches native setup config"), Options.ToNative().bCaptureApplicationLifecycleEvents);
    }
    ~FLifecycleScenario() { Destroy(); GetMutableDefault<UPostHogSettings>()->bAutoInitialize = PreviousAuto; }
    bool Update() override
    {
        if (Host.IsEmpty() || FPlatformTime::Seconds() > Deadline)
        { Test->AddError(FString::Printf(TEXT("Lifecycle scenario requires the native mock driver and must complete in 60 seconds (case %d, phase %d)"), static_cast<int32>(Case), Phase)); return true; }
        const int32 Progress = static_cast<int32>(Case) * 100 + Phase;
        if (Progress != LastProgress)
        {
            LastProgress = Progress;
            Test->AddInfo(FString::Printf(TEXT("Lifecycle case %d phase %d (%.2fs elapsed)"), static_cast<int32>(Case), Phase,
                FPlatformTime::Seconds() - (Deadline - 60)));
        }
        if (GFrameCounter < WaitFrame) { return false; }
        auto* Owner = Instance ? Instance->GetSubsystem<UPostHogSubsystem>() : nullptr;
        switch (Phase)
        {
        case 0:
            Begin(); ++Phase; return false;
        case 1:
            if (bStoppedInHook) { Phase = 8; return false; }
            if (!Owner || !Owner->IsInitialized()) { return false; }
            Client = Owner->GetClient();
            Test->TestEqual(TEXT("ready emitted once on the game thread"), Probe->SetupNotifications, 1);
            Test->TestTrue(TEXT("successful readiness"), Probe->bSetupSucceeded && Probe->bGameThread);
            Expect(Startup());
            Client->Capture(TEXT("lifecycle_signal_marker"));
            Drain = Client->FlushAsync(); ++Phase; return false;
        case 2:
            if (!Drain.IsReady()) { return false; }
            Test->TestEqual(TEXT("manual replay request remains available"), Client->StartSessionReplay(), EPostHogResult::Ok);
            if (Case != ELifecycleCase::InitialBackground && Case != ELifecycleCase::OptedOut) { FrameAllowed(true); }
            Signal(Case != ELifecycleCase::InitialBackground); ++Phase; return false;
        case 3:
            Expect(Startup()); // Duplicate activation, or duplicate initial background.
            Signal(false);
            if (Case == ELifecycleCase::BackgroundShutdown) { Signal(true); }
            ++Phase; return false;
        case 4:
            if (bStoppedInHook) { Phase = 8; return false; }
            Expect(Background());
            if (Case == ELifecycleCase::Disabled) { FrameAllowed(false); }
            Signal(false); ++Phase; return false;
        case 5:
            Expect(Background());
            Signal(true);
            if (Case == ELifecycleCase::ForegroundDestroy) { Signal(false); }
            ++Phase; return false;
        case 6:
            if (bStoppedInHook) { Phase = 8; return false; }
            Expect(Returned());
            if (Case == ELifecycleCase::Disabled) { FrameAllowed(true); }
            if (Case == ELifecycleCase::OptedOut)
            {
                Test->TestEqual(TEXT("SDK opt-in"), Owner->OptIn(), EPostHogResult::Ok);
                Signal(false); Phase = 10; return false;
            }
            Drain = Client->FlushAsync(); ++Phase; return false;
        case 7:
            if (!Drain.IsReady()) { return false; }
            Owner->Shutdown(); Signal(false); Signal(true); Phase = 8; return false;
        case 8:
            if (Owner && Owner->GetState() == EPostHogSubsystemState::ShuttingDown) { return false; }
            if (bStoppedInHook)
            {
                const bool bStartup = Case == ELifecycleCase::StartupShutdown || Case == ELifecycleCase::StartupDestroy;
                Test->TestEqual(TEXT("shutdown hook cannot publish stale readiness"), Probe->SetupNotifications, bStartup ? 0 : 1);
                Test->TestTrue(TEXT("hook close reports Deferred without waiting"), HookClose.IsReady() && HookClose.Get().Status == EPostHogResult::Deferred);
                Expect(bStartup ? TEXT("opened:false") :
                    (Case == ELifecycleCase::BackgroundShutdown ? TEXT("opened:false,background") : TEXT("opened:false,background,opened:true")));
            }
            Test->TestFalse(TEXT("lifecycle hooks run on game thread"), bWrongThread.load());
            Joined = Client->ShutdownAsync(); ++Phase; return false;
        case 9:
            if (!Joined.IsReady()) { return false; }
            Test->TestTrue(TEXT("lifecycle close joins native/host work"), Joined.Get().bJoined);
            Test->TestEqual(TEXT("closed client denies new work"), Client->Capture(TEXT("lifecycle_after_close")), EPostHogResult::Closed);
            Destroy(); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Client.Reset(); Probe.Reset(); Seen.Empty(); Session.Empty(); bStoppedInHook = false;
            HookClose = {}; Joined = {}; Drain = {}; Phase = 0;
            Case = static_cast<ELifecycleCase>(static_cast<uint8>(Case) + 1);
            return Case == ELifecycleCase::Count;
        case 10:
            Expect(TEXT("background")); Signal(true); ++Phase; return false;
        case 11:
            Expect(TEXT("background,opened:true")); Drain = Client->FlushAsync(); Phase = 7; return false;
        }
        return false;
    }
private:
    void Begin()
    {
        Instance.Reset(NewObject<UGameInstance>(GEngine)); Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
        auto* Owner = Instance->GetSubsystem<UPostHogSubsystem>(); Probe.Reset(NewObject<UPostHogBlueprintProbe>());
        Owner->OnSetupCompleted.AddDynamic(Probe.Get(), &UPostHogBlueprintProbe::OnSetup);
        if (Case == ELifecycleCase::Disabled)
        {
            FPostHogOptions Options; Options.ProjectToken = TEXT("phc_MOCK"); Options.Host = Host;
            Options.ClientName = FGuid::NewGuid().ToString(); Options.bEnableInEditor = true;
            Options.bCaptureApplicationLifecycleEvents = false; Options.AppBuild = TEXT("lifecycle-disabled");
            struct { FPostHogOptions Options; bool ReturnValue; } Params{Options, false};
            Owner->ProcessEvent(Owner->FindFunctionChecked(TEXT("Setup")), &Params);
            Test->TestTrue(TEXT("reflected lifecycle-disabled setup"), Params.ReturnValue);
        }
        else
        {
            FPostHogConfig Config; Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = Host;
            Config.ClientName = FGuid::NewGuid().ToString();
            Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogLifecycleTests") / Config.ClientName;
            Config.bInitiallyOptedOut = Case == ELifecycleCase::OptedOut;
            Config.Context.SetString(TEXT("lifecycle_test_case"), FString::FromInt(static_cast<int32>(Case)));
            Config.Context.SetString(TEXT("$app_version"), TEXT("lifecycle-test-version"));
            Config.BeforeSend = [this](FJsonObject& Event) { return Observe(Event); };
            Test->TestTrue(TEXT("lifecycle setup"), Owner->SetupWithConfig(MoveTemp(Config), true));
        }
        // Queue real delegate signals before readiness. Only the resulting state is live at setup.
        Signal(true); Signal(false); Signal(Case != ELifecycleCase::InitialBackground);
    }
    bool Observe(FJsonObject& Event)
    {
        if (!IsInGameThread()) { bWrongThread.store(true); return false; }
        const FString Name = Event.GetStringField(TEXT("event"));
        if (Name != TEXT("Application Opened") && Name != TEXT("Application Backgrounded")) { return true; }
        const auto Properties = Event.GetObjectField(TEXT("properties"));
        if (Name == TEXT("Application Opened"))
        {
            bool bFromBackground = false;
            Test->TestTrue(TEXT("opened carries a boolean from_background"), Properties->TryGetBoolField(TEXT("from_background"), bFromBackground));
            Seen.Add(bFromBackground ? TEXT("opened:true") : TEXT("opened:false"));
        }
        else { Seen.Add(TEXT("background")); }
        Test->TestEqual(TEXT("sampled context retained"), Properties->GetStringField(TEXT("$app_version")), FString(TEXT("lifecycle-test-version")));
        FString EventSession;
        Test->TestTrue(TEXT("automatic foreground lifecycle has SDK session attribution"),
            Properties->TryGetStringField(TEXT("$session_id"), EventSession) && !EventSession.IsEmpty());
        if (Session.IsEmpty()) { Session = EventSession; }
        else { Test->TestEqual(TEXT("short lifecycle transitions preserve the SDK session"), EventSession, Session); }
        const bool bStartupStop = (Case == ELifecycleCase::StartupShutdown || Case == ELifecycleCase::StartupDestroy) && Seen.Num() == 1;
        const bool bBackgroundStop = Case == ELifecycleCase::BackgroundShutdown && Name == TEXT("Application Backgrounded");
        const bool bForegroundStop = Case == ELifecycleCase::ForegroundDestroy && Name == TEXT("Application Opened") && Seen.Num() == 3;
        if (!bStoppedInHook && (bStartupStop || bBackgroundStop || bForegroundStop))
        {
            bStoppedInHook = true; auto* Owner = Instance->GetSubsystem<UPostHogSubsystem>(); Client = Owner->GetClient();
            Test->TestTrue(TEXT("hook owns a ready client"), Client.IsValid());
            if (Case == ELifecycleCase::StartupDestroy || Case == ELifecycleCase::ForegroundDestroy)
            { Destroy(); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS); }
            else { Owner->Shutdown(); }
            HookClose = Client->ShutdownAsync();
            // These are later real platform notifications, not permission to resume after close.
            Signal(false); Signal(true);
        }
        return Case != ELifecycleCase::Dropped;
    }
    void Signal(bool bForeground)
    {
        // One case covers activation aliases; their global listeners restart/stop engine messaging.
        // The remaining cases still use real Core + Slate signals, including duplicate notifications.
        if (bForeground)
        {
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast();
            if (Case == ELifecycleCase::Foreground) { FCoreDelegates::ApplicationHasReactivatedDelegate.Broadcast(); }
        }
        else
        {
            FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Broadcast();
            if (Case == ELifecycleCase::Foreground) { FCoreDelegates::ApplicationWillDeactivateDelegate.Broadcast(); }
        }
        if (FSlateApplication::IsInitialized()) { FSlateApplication::Get().OnApplicationActivationStateChanged().Broadcast(bForeground); }
        WaitFrame = GFrameCounter + 2;
    }
    void FrameAllowed(bool bAllowed)
    {
        TUniquePtr<FPostHogReplayFrame> Frame;
        Test->TestEqual(TEXT("foreground replay admission independent of lifecycle analytics"), Client->BeginReplayFrame(32, 24, 128, Frame),
            bAllowed ? EPostHogResult::Ok : EPostHogResult::Inactive);
        if (Frame) { Frame->Abandon(); }
    }
    FString Startup() const
    { return Case == ELifecycleCase::InitialBackground || Case == ELifecycleCase::Disabled || Case == ELifecycleCase::OptedOut ? TEXT("") : TEXT("opened:false"); }
    FString Background() const
    {
        if (Case == ELifecycleCase::Disabled || Case == ELifecycleCase::OptedOut || Case == ELifecycleCase::InitialBackground) { return TEXT(""); }
        return TEXT("opened:false,background");
    }
    FString Returned() const
    {
        if (Case == ELifecycleCase::Disabled || Case == ELifecycleCase::OptedOut) { return TEXT(""); }
        if (Case == ELifecycleCase::InitialBackground) { return TEXT("opened:true"); }
        return TEXT("opened:false,background,opened:true");
    }
    void Expect(const FString& Expected) { Test->TestEqual(FString::Printf(TEXT("lifecycle case %d sequence"), static_cast<int32>(Case)), FString::Join(Seen, TEXT(",")), Expected); }
    void Destroy()
    {
        if (!Instance) { return; }
        UWorld* World = Instance->GetWorld(); Instance->Shutdown();
        if (World) { World->SetGameInstance(nullptr); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        Instance.Reset();
    }
    FAutomationTestBase* Test;
    FString Host, Session;
    ELifecycleCase Case = ELifecycleCase::Foreground;
    int32 Phase = 0, LastProgress = -1;
    uint64 WaitFrame = 0;
    double Deadline;
    bool PreviousAuto = false, bStoppedInHook = false;
    std::atomic<bool> bWrongThread{false};
    TStrongObjectPtr<UGameInstance> Instance;
    TStrongObjectPtr<UPostHogBlueprintProbe> Probe;
    FPostHogClient::FPtr Client;
    TArray<FString> Seen;
    TSharedFuture<FPostHogDrainResult> Drain, Joined, HookClose;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogLifecycleTest, "PostHog.Lifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogLifecycleTest::RunTest(const FString&)
{ FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FLifecycleScenario>(this)); return true; }
#endif
