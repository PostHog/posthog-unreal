#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Analytics.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/CoreDelegates.h"
#include "Misc/AutomationTest.h"
#include "HAL/PlatformTime.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
struct FAnalyticsHookState
{
    TWeakPtr<IAnalyticsProvider> Provider;
    TWeakPtr<FPostHogClient, ESPMode::ThreadSafe> Client;
    bool bDropStart = false, bReenterStart = false, bCloseStart = false, bRestartAtEnd = false;
    bool bRestartResult = true;
    FString ExpiredEndId, CurrentIdDuringExpiredEnd;
    bool bNestedStart = true, bCloseDeferred = false;
};
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
        Config.SessionMode = EPostHogSessionMode::Manual;
        Config.FlushAt = 100; Config.FlushIntervalMilliseconds = 60000;
        Config.Context.SetString(TEXT("test_suite"), TEXT("analytics"));
        Config.Context.SetString(TEXT("context_wins"), TEXT("context"));
        auto Hook = Hooks;
        Config.BeforeSend = [Hook](FJsonObject& Event)
        {
            Event.GetObjectField(TEXT("properties"))->RemoveField(TEXT("secret"));
            if (Event.GetStringField(TEXT("event")) == TEXT("SessionStart"))
            {
                if (Hook->bDropStart) { Hook->bDropStart = false; return false; }
                if (Hook->bReenterStart)
                { Hook->bReenterStart = false; if (auto Provider = Hook->Provider.Pin()) { Hook->bNestedStart = Provider->StartSession(); } }
                if (Hook->bCloseStart)
                {
                    Hook->bCloseStart = false;
                    if (auto Client = Hook->Client.Pin())
                    { const auto Closing = Client->ShutdownAsync(); Hook->bCloseDeferred = Closing.IsReady() && Closing.Get().Status == EPostHogResult::Deferred; }
                }
            }
            if (Event.GetStringField(TEXT("event")) == TEXT("SessionEnd") && Event.GetObjectField(TEXT("properties"))->HasField(TEXT("expiry_fixture")))
            {
                Hook->ExpiredEndId = Event.GetObjectField(TEXT("properties"))->GetStringField(TEXT("$session_id"));
                if (auto Client = Hook->Client.Pin()) { Hook->CurrentIdDuringExpiredEnd = Client->PeekSessionId(); }
            }
            if (Event.GetStringField(TEXT("event")) == TEXT("SessionEnd") && Hook->bRestartAtEnd)
            { Hook->bRestartAtEnd = false; if (auto Provider = Hook->Provider.Pin()) { Hook->bRestartResult = Provider->StartSession(); } }
            return true;
        };
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
            Hooks->Provider = First; Hooks->Client = FirstClient; Hooks->bDropStart = true;
            Check(!First->StartSession(), TEXT("dropped session annotation reports failure"));
            Hooks->bReenterStart = true;
            Check(First->StartSession(MakeAnalyticsEventAttributeArray(TEXT("chapter"), TEXT("intro"))), TEXT("start session"));
            Check(!Hooks->bNestedStart, TEXT("pending reentrant start cannot falsely report an admitted annotation"));
            Session = First->GetSessionID();
            Check(!Session.IsEmpty() && Session == FirstClient->GetSessionId(), TEXT("adapter and native client share session"));
            Check(First->StartSession(MakeAnalyticsEventAttributeArray(TEXT("chapter"), TEXT("duplicate"))), TEXT("repeated start is idempotent"));
            First->RecordEvent(TEXT("analytics_during"));
            FirstClient->Unregister(TEXT("registered_wins"));
            First->RecordEvent(TEXT("analytics_revealed_default"));
            FirstClient->Register(Registered);
            Check(!First->SetSessionID(TEXT("invalid")), TEXT("invalid session ID rejected"));
            Check(First->GetSessionID() == Session, TEXT("invalid setter retains session"));
            Hooks->bRestartAtEnd = true; First->EndSession();
            Check(!Hooks->bRestartResult && !First->GetSessionID().IsEmpty() && First->GetSessionID() != Session,
                TEXT("reentrant end/start cannot falsely admit or clear the newer SDK session"));
            First->RecordEvent(TEXT("analytics_reentrant_session"));
            First->EndSession(); First->EndSession();
            Check(First->GetSessionID().IsEmpty(), TEXT("explicit end clears session"));
            First->RecordEvent(TEXT("analytics_after"));
            const FString CustomSession = TEXT("01912345-6789-7abc-8def-0123456789ab");
            Check(!First->SetSessionID(CustomSession) && FirstClient->PeekSessionId().IsEmpty(), TEXT("custom session IDs are unsupported without changing SDK state"));
            Check(First->StartSession(), TEXT("SDK creates the next manual session"));
            First->RecordEvent(TEXT("analytics_next_session"));
            First->EndSession();
            First->SetDefaultEventAttributes(MakeAnalyticsEventAttributeArray(TEXT("replacement"), true));
            First->RecordEvent(TEXT("analytics_replaced_defaults"));
            First->SetDefaultEventAttributes({});
            Check(First->GetDefaultEventAttributeCount() == 0, TEXT("defaults clear"));
            First->RecordEvent(TEXT("analytics_cleared_defaults"));
            FirstClient->Capture(TEXT("analytics_native_after_defaults"));

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
            Check(FirstFlush.Get().Status == EPostHogResult::Ok && SecondFlush.Get().Status == EPostHogResult::Ok, TEXT("flush SDK reports"));
            Check(FirstClient->OptOut() == EPostHogResult::Ok, TEXT("opt-out invocation"));
            First->RecordEvent(TEXT("analytics_opted_out"));
            Check(!First->StartSession(), TEXT("opt-out applies to session start"));
            First->SetUserID(TEXT("ignored"));
            Check(First->GetUserID() == TEXT("analytics-user"), TEXT("opt-out applies to identity"));
            ++Phase; return false;
        case 3:
            Check(FirstClient->OptIn() == EPostHogResult::Ok, TEXT("opt-in invocation")); ++Phase; return false;
        case 4:
        {
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
            Hooks->Provider = Replacement; Hooks->Client = FirstClient; Hooks->bCloseStart = true;
            Check(!Replacement->StartSession() && Hooks->bCloseDeferred, TEXT("callback close is deferred and cannot report a live session start"));
            Destroy(One);
            Check(!Factory() && Replacement->GetUserID().IsEmpty(), TEXT("owner destruction unbinds and closes retained adapters"));
            Transition = FirstClient->ShutdownAsync(); ++Phase; return false;
        }
        case 7:
            if (!Transition.IsReady()) { return false; }
            Destroy(Two); FirstClient.Reset(); SecondClient.Reset();
            Config.ClientName = TEXT("analytics-expiry"); Config.StorageRoot = Root / TEXT("expiry");
            Config.SessionMode = EPostHogSessionMode::Automatic; Config.bPersistSessions = true;
            Config.Context.SetBoolean(TEXT("expiry_fixture"), true);
            Two = Create(); B = Two->GetSubsystem<UPostHogSubsystem>();
            Check(B->SetupWithConfig(Config, true), TEXT("persistent automatic expiry fixture setup"));
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast(); ++Phase; return false;
        case 8:
            if (!B->IsInitialized()) { return false; }
            SecondClient = B->GetClient(); SecondClient->SetAppInBackground(false);
            ExpiringId = SecondClient->GetSessionId();
            Check(!ExpiringId.IsEmpty(), TEXT("expiry fixture uses an SDK-created ID"));
            Check(SecondClient->Capture(TEXT("analytics_expiry_seed")) == EPostHogResult::Accepted, TEXT("record the SDK-issued fixture session"));
            SecondFlush = SecondClient->FlushAsync(); ++Phase; return false;
        case 9:
            if (!SecondFlush.IsReady()) { return false; }
            Check(SecondFlush.Get().State == EPostHogPersistenceOutcome::Persisted, TEXT("session persistence completed before fixture rewrite"));
            B->Shutdown(); ++Phase; return false;
        case 10:
        {
            if (B->GetState() != EPostHogSubsystemState::Idle) { return false; }
            SecondClient.Reset();
            TArray<FString> Files;
            IFileManager::Get().FindFilesRecursive(Files, *Config.StorageRoot, TEXT("state.json"), true, false);
            if (Files.Num() != 1) { Test->AddError(TEXT("Expected one isolated SDK state file")); return true; }
            FString Saved; FPostHogProperties Record;
            if (!FFileHelper::LoadFileToString(Saved, *Files[0]) || !FPostHogProperties::TryParse(Saved, Record))
            { Test->AddError(TEXT("Could not read the SDK-created expiry fixture")); return true; }
            auto Envelope = Record.ToNative();
            const auto Stored = Envelope->GetObjectField(TEXT("state"))->GetObjectField(TEXT("session"));
            Check(Envelope->GetIntegerField(TEXT("version")) == 1 && Stored->GetStringField(TEXT("id")) == ExpiringId, TEXT("fixture preserves the real v1 SDK session ID"));
            const int64 NowMs = FDateTime::UtcNow().GetTicks() / ETimespan::TicksPerMillisecond - FDateTime(1970, 1, 1).GetTicks() / ETimespan::TicksPerMillisecond;
            // Restore a still-valid session five seconds before its 24-hour age boundary.
            Stored->SetNumberField(TEXT("started_ms"), NowMs - 24LL * 60 * 60 * 1000 + 5000);
            Stored->SetNumberField(TEXT("activity_ms"), NowMs);
            ExpiresAfter = FPlatformTime::Seconds() + 5.25;
            if (!FFileHelper::SaveStringToFile(FPostHogProperties::FromNative(Envelope).ToJson(), *Files[0], FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
            { Test->AddError(TEXT("Could not write the isolated expiry fixture")); return true; }
            Check(B->SetupWithConfig(Config, true), TEXT("restore the still-valid session through public setup"));
            FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Broadcast(); ++Phase; return false;
        }
        case 11:
            if (!B->IsInitialized()) { return false; }
            SecondClient = B->GetClient(); SecondClient->SetAppInBackground(false);
            Check(SecondClient->PeekSessionId() == ExpiringId, TEXT("real SDK restore retains the not-yet-expired ID"));
            ++Phase; return false;
        case 12:
            if (FPlatformTime::Seconds() < ExpiresAfter) { return false; }
            Check(SecondClient->PeekSessionId() == ExpiringId, TEXT("passive peek still exposes the expired automatic ID"));
            Hooks->Client = SecondClient;
            B->GetAnalyticsProvider()->EndSession();
            Check(Hooks->ExpiredEndId == ExpiringId, TEXT("expired automatic SessionEnd annotates the original SDK session"));
            Check(!Hooks->CurrentIdDuringExpiredEnd.IsEmpty() && Hooks->CurrentIdDuringExpiredEnd != ExpiringId,
                TEXT("SDK actually rotated the expired foreground session before the hook"));
            Check(SecondClient->PeekSessionId().IsEmpty(), TEXT("cleanup ends the SDK-current session after annotation"));
            SecondFlush = SecondClient->FlushAsync(); ++Phase; return false;
        case 13:
            if (!SecondFlush.IsReady()) { return false; }
            B->Shutdown(); ++Phase; return false;
        case 14:
            if (B->GetState() != EPostHogSubsystemState::Idle) { return false; }
            Destroy(Two); SecondClient.Reset(); return true;
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
    TSharedFuture<FPostHogDrainResult> FirstFlush, SecondFlush, Transition;
    TSharedRef<FAnalyticsHookState, ESPMode::ThreadSafe> Hooks = MakeShared<FAnalyticsHookState, ESPMode::ThreadSafe>();
    FString Root, Session, PreviousProvider, ExpiringId;
    double ExpiresAfter = 0;
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
