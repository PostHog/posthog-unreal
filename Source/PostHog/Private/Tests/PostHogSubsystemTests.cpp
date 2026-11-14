#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogSubsystem.h"
#include "PostHogTasks.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
struct FOwnerHookGate
{
    FEvent* Entered = FPlatformProcess::GetSynchEventFromPool(true);
    FEvent* Release = FPlatformProcess::GetSynchEventFromPool(true);
    ~FOwnerHookGate() { FPlatformProcess::ReturnSynchEventToPool(Entered); FPlatformProcess::ReturnSynchEventToPool(Release); }
};
class FSubsystemScenario final : public IAutomationLatentCommand
{
public:
    explicit FSubsystemScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>(); PreviousAuto = Settings->bAutoInitialize; Settings->bAutoInitialize = false;
        Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogSubsystemTests") / FGuid::NewGuid().ToString();
        Config.bPreloadFeatureFlags = false; Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FSubsystemScenario()
    {
        if (Gate) { Gate->Release->Trigger(); }
        Destroy(One); Destroy(Two); Destroy(Cancelled); Destroy(Replacement);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = PreviousAuto;
    }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("subsystem ownership scenario timed out")); return true; }
        switch (Phase)
        {
        case 0:
        {
            if (Config.Host.IsEmpty()) { Test->AddError(TEXT("Set POSTHOG_TEST_HOST")); return true; }
            Create(One); Create(Two); auto* A = One->GetSubsystem<UPostHogSubsystem>(); auto* B = Two->GetSubsystem<UPostHogSubsystem>();
            Check(!A->SetupWithConfig(Config), TEXT("editor guard"));
            Two->GetWorldContext()->RunAsDedicated = true; Check(!B->SetupWithConfig(Config, true), TEXT("dedicated server guard")); Two->GetWorldContext()->RunAsDedicated = false;
            auto First = Config; First.ClientName = TEXT("one"); auto Second = Config; Second.ClientName = TEXT("two");
            Check(A->SetupWithConfig(First, true) && B->SetupWithConfig(Second, true), TEXT("two isolated game instances"));
            Check(!A->SetupWithConfig(First, true), TEXT("initializing setup rejected")); ++Phase; return false;
        }
        case 1:
        {
            auto* A = One->GetSubsystem<UPostHogSubsystem>(); auto* B = Two->GetSubsystem<UPostHogSubsystem>();
            if (!A->IsInitialized() || !B->IsInitialized()) { return false; }
            FirstClient = A->GetClient(); SecondClient = B->GetClient();
            Check(FirstClient->GetDistinctId() != SecondClient->GetDistinctId(), TEXT("namespace inputs isolate identity"));
            const FString Identity = FirstClient->GetDistinctId(); UWorld* Previous = One->GetWorld();
            auto* Next = UWorld::CreateWorld(EWorldType::Game, false, FName(*FGuid::NewGuid().ToString()));
            Next->SetGameInstance(One.Get()); One->GetWorldContext()->SetCurrentWorld(Next); Previous->SetGameInstance(nullptr); Previous->DestroyWorld(false);
            Check(One->GetSubsystem<UPostHogSubsystem>() == A && A->GetClient()->GetDistinctId() == Identity, TEXT("level/world replacement keeps the instance client"));
            FirstClient->Capture(TEXT("subsystem_one")); SecondClient->Capture(TEXT("subsystem_two"));
            FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Broadcast(); ++Phase; return false;
        }
        case 2:
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast();
            One->GetSubsystem<UPostHogSubsystem>()->Shutdown(); Two->GetSubsystem<UPostHogSubsystem>()->Shutdown();
            Check(FirstClient->Capture(TEXT("after-owner-close")) == EPostHogResult::Closed, TEXT("retained pointer loses admission immediately"));
            ++Phase; return false;
        case 3:
            if (One->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle || Two->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Destroy(One); Destroy(Two); FirstClient.Reset(); SecondClient.Reset(); Create(Cancelled);
            Check(Cancelled->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Config, true), TEXT("start initializer"));
            Destroy(Cancelled); CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
            Create(Replacement); Check(Replacement->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Config, true), TEXT("replacement waits cancelled namespace owner"));
            ++Phase; return false;
        case 4:
            if (!Replacement->GetSubsystem<UPostHogSubsystem>()->IsInitialized()) { return false; }
            Replacement->GetSubsystem<UPostHogSubsystem>()->GetClient()->Capture(TEXT("subsystem_replacement"));
            Replacement->GetSubsystem<UPostHogSubsystem>()->Shutdown(); ++Phase; return false;
        case 5:
            if (Replacement->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
        {
            Destroy(Replacement); Create(One); Create(Two);
            // Pure SDK key checks, followed by disabled cloud clients: never contact cloud endpoints.
            auto Cloud = Config; Cloud.ClientName = TEXT("cloud-owner"); Cloud.ProjectToken = TEXT("phx_disabled_test");
            FString Canonical; Cloud.Host = TEXT("https://us.i.posthog.com");
            Check(FPostHogClient::CoordinationKey(Cloud, Canonical) == EPostHogResult::Ok, TEXT("SDK coordination key"));
            for (const FString& Host : {FString(), FString(TEXT(" https://app.posthog.com/// ")), FString(TEXT("https://us.posthog.com/")), FString(TEXT("\thttps://us.i.posthog.com///\n"))})
            {
                Cloud.Host = Host; Cloud.ProjectToken = TEXT("\u2003phx_disabled_test \t"); FString Alias;
                Check(FPostHogClient::CoordinationKey(Cloud, Alias) == EPostHogResult::Ok && Alias == Canonical, TEXT("SDK cloud aliases/default/whitespace share coordination key"));
            }
            Cloud.Host = TEXT("https://us.posthog.com"); Cloud.ProjectToken = TEXT("phx_disabled_test");
            Check(One->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Cloud, true), TEXT("cloud owner without network"));
            ++Phase; return false;
        }
        case 6:
        {
            auto* A = One->GetSubsystem<UPostHogSubsystem>(); if (!A->IsInitialized()) { return false; }
            Check(A->IsDisabled(), TEXT("invalid test token disables cloud network"));
            for (const FString& Host : {FString(), FString(TEXT(" https://app.posthog.com/// ")), FString(TEXT("https://us.i.posthog.com/")), FString(TEXT("https://us.posthog.com"))})
            {
                auto Cloud = Config; Cloud.ClientName = TEXT("cloud-owner"); Cloud.Host = Host; Cloud.ProjectToken = TEXT(" phx_disabled_test\t");
                Check(!Two->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Cloud, true), TEXT("equivalent cloud inputs reject a second live engine owner"));
            }
            A->Shutdown(); ++Phase; return false;
        }
        case 7:
        {
            if (One->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Destroy(One); Destroy(Two); Create(Cancelled); Create(Replacement);
            Gate = MakeShared<FOwnerHookGate, ESPMode::ThreadSafe>(); auto HookGate = Gate;
            auto OwnerConfig = Config; OwnerConfig.ClientName = TEXT("normalized-owner");
            OwnerConfig.BeforeSend = [HookGate](FJsonObject& Event)
            {
                if (Event.GetStringField(TEXT("event")) == TEXT("hold-owner")) { HookGate->Entered->Trigger(); HookGate->Release->Wait(); }
                return true;
            };
            Check(Cancelled->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(MoveTemp(OwnerConfig), true), TEXT("persistent replacement predecessor"));
            ++Phase; return false;
        }
        case 8:
        {
            auto* A = Cancelled->GetSubsystem<UPostHogSubsystem>(); if (!A->IsInitialized()) { return false; }
            FirstClient = A->GetClient(); Check(FirstClient->Identify(TEXT("normalized-owner-id")) == EPostHogResult::Ok, TEXT("identify predecessor"));
            for (const FString& Host : {Config.Host, Config.Host + TEXT("///"), TEXT(" \t") + Config.Host + TEXT("/\n")})
            {
                auto Equivalent = Config; Equivalent.ClientName = TEXT("normalized-owner"); Equivalent.Host = Host; Equivalent.ProjectToken = TEXT("\u2003phc_MOCK\t");
                Check(!Replacement->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Equivalent, true), TEXT("slash/whitespace inputs reject a second live engine owner"));
            }
            Anonymous = FirstClient->GetAnonymousId(); auto Current = FirstClient;
            HeldCapture = PostHogTasks::Run([Current] { return Current->Capture(TEXT("hold-owner")); });
            ++Phase; return false;
        }
        case 9:
        {
            if (!Gate->Entered->Wait(0)) { return false; }
            Check(FirstClient->OptOut() == EPostHogResult::Ok && FirstClient->IsOptedOut(), TEXT("consent changes while permitted hook is held"));
            PendingRequest = Cancelled->GetSubsystem<UPostHogSubsystem>()->GetInitializationForTests();
            Cancelled->GetSubsystem<UPostHogSubsystem>()->Shutdown(); OwnerClose = FirstClient->ShutdownAsync();
            UPostHogSubsystem::RetireClosedInitializationOwnersForTests();
            Check(PendingRequest.IsValid(), TEXT("pending owner remains strongly held during retirement"));
            Check(!OwnerClose.IsReady(), TEXT("native close is held by active hook"));
            auto Equivalent = Config; Equivalent.ClientName = TEXT("normalized-owner");
            Equivalent.Host = TEXT(" \t") + Config.Host + TEXT("///\n"); Equivalent.ProjectToken = TEXT("\u2003phc_MOCK\t");
            Check(Replacement->GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Equivalent, true), TEXT("equivalent replacement starts behind cancelled predecessor"));
            ++Phase; return false;
        }
        case 10:
        {
            auto* Next = Replacement->GetSubsystem<UPostHogSubsystem>();
            if (!Next->IsWaitingForPredecessorForTests())
            {
                if (Next->GetState() != EPostHogSubsystemState::Initializing)
                { Check(false, TEXT("replacement bypassed predecessor join")); Gate->Release->Trigger(); return true; }
                return false;
            }
            UPostHogSubsystem::RetireClosedInitializationOwnersForTests();
            Check(!OwnerClose.IsReady() && !HeldCapture.IsReady() && !Next->GetClient(), TEXT("retirement preserves initializing replacement and pending predecessor coordination"));
            Gate->Release->Trigger(); ++Phase; return false;
        }
        case 11:
        {
            auto* Next = Replacement->GetSubsystem<UPostHogSubsystem>();
            if (!Next->IsInitialized() || !OwnerClose.IsReady() || !HeldCapture.IsReady()) { return false; }
            Check(OwnerClose.Get().bJoined, TEXT("predecessor native close completed"));
            auto Restored = Next->GetClient();
            Check(Restored->GetDistinctId() == TEXT("normalized-owner-id") && Restored->GetAnonymousId() == Anonymous, TEXT("replacement restores new Rust storage identity"));
            Check(Restored->IsOptedOut(), TEXT("replacement restores consent instead of memory fallback default"));
            ClosedRequest = Next->GetInitializationForTests();
            Next->Shutdown(); ++Phase; return false;
        }
        case 12:
            if (Replacement->GetSubsystem<UPostHogSubsystem>()->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Check(ClosedRequest.IsValid(), TEXT("closed initialization owner is retained until explicit retirement"));
            Destroy(Cancelled); Destroy(Replacement); FirstClient.Reset();
            UPostHogSubsystem::RetireClosedInitializationOwnersForTests(); ++Phase; return false;
        case 13:
            if (PendingRequest.IsValid() || ClosedRequest.IsValid()) { return false; }
            Check(!PendingRequest.IsValid() && !ClosedRequest.IsValid(), TEXT("joined owner retirement releases initialization client/future holders")); return true;
        }
        return false;
    }
private:
    void Create(TStrongObjectPtr<UGameInstance>& Instance)
    { Instance.Reset(NewObject<UGameInstance>(GEngine)); Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString())); }
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
    bool PreviousAuto = false;
    int32 Phase = 0;
    double Deadline;
    TStrongObjectPtr<UGameInstance> One, Two, Cancelled, Replacement;
    FPostHogClient::FPtr FirstClient, SecondClient;
    FString Anonymous;
    TSharedPtr<FOwnerHookGate, ESPMode::ThreadSafe> Gate;
    TSharedFuture<EPostHogResult> HeldCapture;
    TSharedFuture<FPostHogDrainResult> OwnerClose;
    TWeakPtr<FPostHogInitialization, ESPMode::ThreadSafe> PendingRequest, ClosedRequest;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogSubsystemTest, "PostHog.Subsystem", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogSubsystemTest::RunTest(const FString&)
{ FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FSubsystemScenario>(this)); return true; }
#endif
