#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Analytics.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
class FAnalyticsScenario final : public IAutomationLatentCommand
{
public:
    explicit FAnalyticsScenario(FAutomationTestBase* InTest) : Test(InTest)
    {
        auto* Settings = GetMutableDefault<UPostHogSettings>();
        bPreviousAuto = Settings->bAutoInitialize; Settings->bAutoInitialize = false;
        bHadProvider = GConfig->GetString(TEXT("Analytics"), TEXT("ProviderModuleName"), PreviousProvider, GEngineIni);
        Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogAnalyticsTests") / FGuid::NewGuid().ToString());
        Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
        Config.StorageRoot = Root; Config.ClientName = TEXT("analytics-one");
        Config.bPreloadFeatureFlags = false; Config.bCaptureApplicationLifecycleEvents = false;
        Config.SessionMode = EPostHogSessionMode::Explicit;
        Config.FlushAt = 100; Config.FlushIntervalMilliseconds = 60000;
        Config.Context.SetString(TEXT("test_suite"), TEXT("analytics"));
        Config.Context.SetString(TEXT("context_wins"), TEXT("context"));
        Config.BeforeSend = [](FPostHogEvent& Event) { Event.Properties.Remove(TEXT("secret")); return true; };
        Deadline = FPlatformTime::Seconds() + 30;
    }
    ~FAnalyticsScenario()
    {
        Destroy(One); Destroy(Two);
        GetMutableDefault<UPostHogSettings>()->bAutoInitialize = bPreviousAuto;
        if (bHadProvider) { GConfig->SetString(TEXT("Analytics"), TEXT("ProviderModuleName"), *PreviousProvider, GEngineIni); }
        else { GConfig->RemoveKey(TEXT("Analytics"), TEXT("ProviderModuleName"), GEngineIni); }
    }
    bool Update() override
    {
        if (FPlatformTime::Seconds() > Deadline) { Test->AddError(TEXT("Analytics scenario timed out")); return true; }
        auto* A = One ? One->GetSubsystem<UPostHogSubsystem>() : nullptr;
        auto* B = Two ? Two->GetSubsystem<UPostHogSubsystem>() : nullptr;
        switch (Phase)
        {
        case 0:
        {
            if (Config.Host.IsEmpty()) { Test->AddError(TEXT("Run with Tests/native_integration.py")); return true; }
            One = Create(); Two = Create(); A = One->GetSubsystem<UPostHogSubsystem>(); B = Two->GetSubsystem<UPostHogSubsystem>();
            Check(!A->GetAnalyticsProvider() && !A->BindAnalyticsProvider(), TEXT("not-ready instances cannot provide or bind adapters"));
            Check(!Factory(), TEXT("factory never creates an implicit client"));
            Check(!FPostHogAnalyticsProvider::GetForWorld(nullptr), TEXT("null context is harmless"));
            Check(A->SetupWithConfig(Config, true), TEXT("explicit owner setup"));
            auto Automatic = Config; Automatic.ClientName = TEXT("analytics-two"); Automatic.SessionMode = EPostHogSessionMode::Automatic;
            Check(B->SetupWithConfig(Automatic, true), TEXT("automatic owner setup"));
            ++Phase; return false;
        }
        case 1:
        {
            if (!A->IsInitialized() || !B->IsInitialized()) { return false; }
            First = A->GetAnalyticsProvider(); Second = B->GetAnalyticsProvider();
            FirstClient = A->GetClient(); SecondClient = B->GetClient();
            Check(First && Second && First != Second, TEXT("separate game instances have separate adapters"));
            if (!First || !Second) { return true; }
            Check(First == FPostHogAnalyticsProvider::GetForWorld(One->GetWorld()), TEXT("world resolves its own adapter"));
            Check(Second == FPostHogAnalyticsProvider::GetForWorld(Two->GetWorld()), TEXT("second world resolves its own adapter"));
            Check(First == A->GetAnalyticsProvider() && !Factory(), TEXT("adapter stable; no implicit binding"));

            FPostHogProperties Registered;
            Registered.SetString(TEXT("registered_wins"), TEXT("registered"));
            Registered.SetString(TEXT("context_wins"), TEXT("registered"));
            Registered.SetBoolean(TEXT("registered_only"), true);
            FirstClient->Register(Registered);
            First->SetDefaultEventAttributes(MakeAnalyticsEventAttributeArray(
                TEXT("precedence"), TEXT("default"), TEXT("registered_wins"), TEXT("default"),
                TEXT("context_wins"), TEXT("default"), TEXT("default_only"), 123));
            Check(First->GetDefaultEventAttributeCount() == 4 && First->GetDefaultEventAttribute(0).GetName() == TEXT("precedence"), TEXT("default attribute accessors"));
            auto DefaultCopy = First->GetDefaultEventAttributesSafe(); DefaultCopy.Empty();
            Check(First->GetDefaultEventAttributeCount() == 4, TEXT("default attributes returned by value"));
            FirstClient->Capture(TEXT("analytics_native"));
            First->RecordEvent(TEXT("analytics_before"), MakeAnalyticsEventAttributeArray(
                TEXT("precedence"), TEXT("caller"), TEXT("string"), TEXT("00123"), TEXT("number"), 42,
                TEXT("boolean"), false, TEXT("object"), FJsonFragment(TEXT("{\"nested\":true}")),
                TEXT("array"), FJsonFragment(TEXT("[null,1,false]")), TEXT("secret"), TEXT("redact"),
                TEXT("invalid"), FJsonFragment(TEXT("not json"))));
            Check(First->GetSessionID().IsEmpty(), TEXT("explicit events before StartSession are sessionless"));
            Check(First->StartSession(MakeAnalyticsEventAttributeArray(TEXT("chapter"), TEXT("intro"))), TEXT("start session"));
            Session = First->GetSessionID();
            Check(!Session.IsEmpty() && Session == FirstClient->GetSessionId(), TEXT("adapter and native client share session"));
            Check(First->StartSession(MakeAnalyticsEventAttributeArray(TEXT("chapter"), TEXT("duplicate"))), TEXT("repeated start is idempotent"));
            First->RecordEvent(TEXT("analytics_during"));
            Check(!First->SetSessionID(TEXT("invalid")), TEXT("invalid session ID rejected"));
            Check(First->GetSessionID() == Session, TEXT("invalid setter retains session"));
            First->EndSession(); First->EndSession();
            Check(First->GetSessionID().IsEmpty(), TEXT("explicit end clears session"));
            First->RecordEvent(TEXT("analytics_after"));
            const FString CustomSession = TEXT("01912345-6789-7abc-8def-0123456789ab");
            Check(First->SetSessionID(CustomSession) && FirstClient->GetSessionId() == CustomSession, TEXT("valid UUIDv7 shares native session"));
            First->RecordEvent(TEXT("analytics_custom_session"));
            First->EndSession();
            First->SetDefaultEventAttributes(MakeAnalyticsEventAttributeArray(TEXT("replacement"), true));
            First->RecordEvent(TEXT("analytics_replaced_defaults"));

            Second->RecordEvent(TEXT("analytics_auto_before"));
            const FString AutoSession = Second->GetSessionID();
            Check(Second->StartSession() && Second->StartSession() && Second->GetSessionID() == AutoSession, TEXT("automatic session is shared without rotation"));
            Second->EndSession();
            Second->RecordEvent(TEXT("analytics_auto_after"));
            Check(Second->GetSessionID() != AutoSession, TEXT("automatic policy resumes on the next event"));

            Check(A->BindAnalyticsProvider() && A->BindAnalyticsProvider(), TEXT("explicit binding is idempotent"));
            Check(!B->BindAnalyticsProvider(), TEXT("second owner cannot steal binding"));
            B->UnbindAnalyticsProvider();
            Check(Factory() == First, TEXT("unbinding another owner cannot clear binding"));
            const auto Conflicting = FAnalyticsProviderConfigurationDelegate::CreateLambda([](const FString& Key, bool) {
                return Key == TEXT("ProjectToken") ? FString(TEXT("phc_OTHER_MOCK")) : FString();
            });
            Check(!FAnalytics::Get().CreateAnalyticsProvider(TEXT("PostHogAnalytics"), Conflicting), TEXT("factory cannot silently ignore routing configuration"));
            GConfig->SetString(TEXT("Analytics"), TEXT("ProviderModuleName"), TEXT("PostHogAnalytics"), GEngineIni);
            Check(FAnalytics::Get().GetDefaultConfiguredProvider() == First, TEXT("standard configured-provider path"));

            // Invoke stock Blueprint nodes through reflection: the engine class has no exported C++ API.
            FModuleManager::LoadModuleChecked<IModuleInterface>(TEXT("AnalyticsBlueprintLibrary"));
            auto* LibraryClass = FindObject<UClass>(nullptr, TEXT("/Script/AnalyticsBlueprintLibrary.AnalyticsBlueprintLibrary"));
            Check(LibraryClass != nullptr, TEXT("stock Analytics Blueprint library loaded"));
            if (!LibraryClass) { return true; }
            auto* Library = LibraryClass->GetDefaultObject();
            struct { FString UserId; } IdentifyParams{TEXT("analytics-user")};
            Library->ProcessEvent(Library->FindFunctionChecked(TEXT("SetUserId")), &IdentifyParams);
            Check(FirstClient->GetDistinctId() == TEXT("analytics-user") && First->GetUserID() == TEXT("analytics-user"), TEXT("stock nodes share native identity"));
            First->SetUserID(TEXT("")); First->SetUserID(TEXT("another-user"));
            Check(First->GetUserID() == TEXT("analytics-user"), TEXT("empty ID and account switch preserve Identify rules"));
            struct { FString EventName; FString AttributeName; FString AttributeValue; }
                EventParams{TEXT("analytics_stock_blueprint"), TEXT("string"), TEXT("00123")};
            Library->ProcessEvent(Library->FindFunctionChecked(TEXT("RecordEventWithAttribute")), &EventParams);
            First->RecordError(TEXT("ordinary-error"));
            First->RecordEvent(TEXT("analytics_immediate"), {}, EAnalyticsRecordEventMode::Immediate);

            const auto MulticastConfig = FAnalyticsProviderConfigurationDelegate::CreateLambda([](const FString& Key, bool) {
                return Key == TEXT("ProviderModuleNames") ? FString(TEXT("PostHogAnalytics")) : FString();
            });
            Multicast = FAnalytics::Get().CreateAnalyticsProvider(TEXT("AnalyticsMulticast"), MulticastConfig);
            Check(Multicast.IsValid(), TEXT("multicast adapter created"));
            if (Multicast) { Multicast->RecordEvent(TEXT("analytics_multicast")); }
            FirstFlush = FirstClient->FlushAsync(); SecondFlush = SecondClient->FlushAsync();
            ++Phase; return false;
        }
        case 2:
            if (!FirstFlush.IsReady() || !SecondFlush.IsReady()) { return false; }
            Check(FirstFlush.Get().bCompleted && SecondFlush.Get().bCompleted, TEXT("flush wire events"));
            Transition = FirstClient->OptOutAsync();
            First->RecordEvent(TEXT("analytics_opted_out"));
            Check(!First->StartSession(), TEXT("opt-out applies to session start"));
            First->SetUserID(TEXT("ignored"));
            Check(First->GetUserID() == TEXT("analytics-user"), TEXT("opt-out applies to identity"));
            ++Phase; return false;
        case 3:
            if (!Transition.IsReady()) { return false; }
            Check(Transition.Get(), TEXT("opt-out completed"));
            Transition = FirstClient->OptInAsync(); ++Phase; return false;
        case 4:
        {
            if (!Transition.IsReady()) { return false; }
            Check(Transition.Get(), TEXT("opt-in completed"));
            UWorld* PreviousWorld = One->GetWorld();
            auto* NextWorld = UWorld::CreateWorld(EWorldType::Game, false, FName(*FGuid::NewGuid().ToString()));
            NextWorld->SetGameInstance(One.Get()); One->GetWorldContext()->SetCurrentWorld(NextWorld);
            PreviousWorld->SetGameInstance(nullptr); PreviousWorld->DestroyWorld(false);
            Check(FPostHogAnalyticsProvider::GetForWorld(NextWorld) == First && Factory() == First, TEXT("world replacement preserves adapter and binding"));
            A->UnbindAnalyticsProvider();
            Check(!Factory() && B->BindAnalyticsProvider() && Factory() == Second, TEXT("explicit unbind permits a new designation"));
            First->RecordEvent(TEXT("analytics_retained_first"));
            if (Multicast) { Multicast->RecordEvent(TEXT("analytics_retained_multicast")); }
            A->Shutdown();
            Check(Factory() == Second, TEXT("old owner shutdown leaves new binding intact"));
            First->RecordEvent(TEXT("analytics_closed"));
            Check(!First->StartSession() && First->GetUserID().IsEmpty() && First->GetSessionID().IsEmpty(), TEXT("retained closed adapter is harmless"));
            Second->RecordEvent(TEXT("analytics_bound_second"));
            B->Shutdown();
            Check(!Factory(), TEXT("bound owner shutdown removes binding"));
            ++Phase; return false;
        }
        case 5:
            if (A->GetState() != EPostHogSubsystemState::Idle || B->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Check(A->SetupWithConfig(Config, true), TEXT("restart owner")); ++Phase; return false;
        case 6:
        {
            if (!A->IsInitialized()) { return false; }
            auto Replacement = A->GetAnalyticsProvider();
            Check(Replacement != First && A->BindAnalyticsProvider() && Factory() == Replacement, TEXT("new setup gets a fresh adapter and explicit binding"));
            Check(First->GetUserID().IsEmpty(), TEXT("old adapter does not revive"));
            Replacement->RecordEvent(TEXT("analytics_restart"));
            FirstClient = A->GetClient();
            Destroy(One);
            Check(!Factory() && Replacement->GetUserID().IsEmpty(), TEXT("owner destruction unbinds and closes retained adapters"));
            Transition = FirstClient->ShutdownAsync(); ++Phase; return false;
        }
        case 7:
            if (!Transition.IsReady()) { return false; }
            Destroy(Two); FirstClient.Reset(); SecondClient.Reset();
            IFileManager::Get().DeleteDirectory(*Root, false, true);
            return true;
        }
        return false;
    }
private:
    static TSharedPtr<IAnalyticsProvider> Factory()
    { return FAnalytics::Get().CreateAnalyticsProvider(TEXT("PostHogAnalytics"), FAnalyticsProviderConfigurationDelegate()); }
    static TStrongObjectPtr<UGameInstance> Create()
    {
        TStrongObjectPtr<UGameInstance> Instance(NewObject<UGameInstance>(GEngine));
        Instance->InitializeStandalone(FName(*FGuid::NewGuid().ToString()));
        return Instance;
    }
    static void Destroy(TStrongObjectPtr<UGameInstance>& Instance)
    {
        if (!Instance) { return; }
        UWorld* World = Instance->GetWorld(); Instance->Shutdown();
        if (World) { World->SetGameInstance(nullptr); GEngine->DestroyWorldContext(World); World->DestroyWorld(false); }
        Instance.Reset();
    }
    void Check(bool Pass, const TCHAR* Message) { if (!Pass) { Test->AddError(Message); } }
    FAutomationTestBase* Test;
    TStrongObjectPtr<UGameInstance> One, Two;
    TSharedPtr<IAnalyticsProvider> First, Second, Multicast;
    FPostHogClient::FPtr FirstClient, SecondClient;
    FPostHogConfig Config;
    TSharedFuture<FPostHogDeliveryResult> FirstFlush, SecondFlush;
    TSharedFuture<bool> Transition;
    FString Root, Session, PreviousProvider;
    bool bPreviousAuto = false, bHadProvider = false;
    int32 Phase = 0;
    double Deadline = 0;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogAnalyticsTest, "PostHog.Analytics",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogAnalyticsTest::RunTest(const FString& Parameters)
{
    AddExpectedError(TEXT("Ignoring an invalid Analytics JSON attribute"), EAutomationExpectedErrorFlags::Contains, 1);
    AddExpectedError(TEXT("Configure PostHog credentials and client options on the subsystem"), EAutomationExpectedErrorFlags::Contains, 1);
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FAnalyticsScenario>(this));
    return true;
}
#endif
