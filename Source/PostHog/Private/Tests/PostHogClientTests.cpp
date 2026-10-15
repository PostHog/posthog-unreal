#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include <limits>
#include "PostHogJson.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogPropertiesTest, "PostHog.Properties",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogPropertiesTest::RunTest(const FString& Parameters)
{
    FPostHogProperties Properties;
    TestTrue(TEXT("String"), Properties.SetString(TEXT("text"), TEXT("玩家 🚀")));
    TestTrue(TEXT("Number"), Properties.SetNumber(TEXT("number"), 42.5));
    TestTrue(TEXT("Boolean"), Properties.SetBoolean(TEXT("boolean"), false));
    FPostHogProperties Copy = Properties;
    Copy.SetString(TEXT("text"), TEXT("copy"));
    TestEqual(TEXT("Value copy"), Properties.ToNative()->GetStringField(TEXT("text")), FString(TEXT("玩家 🚀")));
    FPostHogValue Value;
    TestTrue(TEXT("JSON scalar"), FPostHogValue::TryParse(TEXT("false"), Value));
    TestEqual(TEXT("Boolean scalar type"), Value.ToNative()->Type, EJson::Boolean);
    TestTrue(TEXT("JSON null"), FPostHogValue::TryParse(TEXT("null"), Value));
    TestTrue(TEXT("Null distinct from invalid"), Value.IsValid() && Value.ToNative()->IsNull());
    TestFalse(TEXT("Invalid JSON"), FPostHogValue::TryParse(TEXT("{bad"), Value));
    TestFalse(TEXT("Multiple roots"), FPostHogValue::TryParse(TEXT("1,2"), Value));
    TestFalse(TEXT("Non-object properties"), FPostHogProperties::TryParse(TEXT("[]"), Copy));
    TestFalse(TEXT("NaN"), Properties.SetNumber(TEXT("bad"), std::numeric_limits<double>::quiet_NaN()));
    TestFalse(TEXT("Depth bound"), FPostHogValue::TryParse(FString::ChrN(65, TEXT('[')) + TEXT("0") + FString::ChrN(65, TEXT(']')), Value));
    TestFalse(TEXT("Size bound"), Properties.SetString(TEXT("big"), FString::ChrN(1024 * 1024, TEXT('x'))));
    return true;
}

namespace
{
struct FClientTestResult { TArray<FString> Errors; };
class FWaitForClientTests final : public IAutomationLatentCommand
{
public:
    FWaitForClientTests(FAutomationTestBase* InTest, TFuture<FClientTestResult>&& InFuture) : Test(InTest), Future(MoveTemp(InFuture)) {}
    virtual bool Update() override
    {
        if (!Future.IsReady()) { return false; }
        for (const FString& Error : Future.Get().Errors) { Test->AddError(Error); }
        return true;
    }
private:
    FAutomationTestBase* Test;
    TFuture<FClientTestResult> Future;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientTest, "PostHog.StatefulClient",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogClientTest::RunTest(const FString& Parameters)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Host.IsEmpty()) { AddError(TEXT("Set POSTHOG_TEST_HOST to the SDK mock server.")); return false; }
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogClientTests") / FGuid::NewGuid().ToString());
    auto Future = Async(EAsyncExecution::Thread, [Host, Root]
    {
        FClientTestResult Result;
        auto Check = [&Result](bool bPass, const TCHAR* Message) { if (!bPass) { Result.Errors.Add(Message); } };
        FPostHogConfig Config; Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = Host; Config.StorageRoot = Root;
        Config.FlushAt = 100; Config.FlushIntervalMilliseconds = 60000;
        auto Now = MakeShared<FDateTime, ESPMode::ThreadSafe>(2026, 1, 1);
        Config.Now = [Now] { return *Now; };
        Config.Context.SetString(TEXT("precedence"), TEXT("context"));
        auto Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("create stateful client")); if (!Client) { return Result; }
        const FString Anonymous = Client->GetAnonymousId();
        const FString FirstSession = Client->GetSessionId();
        Check(Anonymous == Client->GetDistinctId(), TEXT("anonymous identity"));
        Check(FirstSession.Len() == 36 && FirstSession[14] == TEXT('7'), TEXT("UUIDv7 session"));
        FPostHogProperties Registered; Registered.SetString(TEXT("registered"), TEXT("saved")); Registered.SetString(TEXT("precedence"), TEXT("registered"));
        Client->Register(Registered);
        FPostHogProperties Properties;
        FPostHogProperties::TryParse(TEXT("{\"precedence\":\"caller\",\"$is_identified\":true,\"$process_person_profile\":true,\"null\":null,\"array\":[null,{\"gone\":null,\"kept\":1}],\"string\":\"00123\"}"), Properties);
        Check(Client->Capture(TEXT("stateful_anonymous"), Properties) == EPostHogCaptureResult::Accepted, TEXT("capture anonymous"));
        Check(Client->Identify(TEXT("user-one")), TEXT("identify anonymous"));
        Check(!Client->Identify(TEXT("user-one")), TEXT("duplicate identify suppressed"));
        Check(!Client->Identify(TEXT("user-two")), TEXT("account switch requires reset"));
        Check(Client->GetSessionId() == FirstSession, TEXT("identify keeps session"));
        FPostHogProperties Person; Person.SetString(TEXT("plan"), TEXT("paid"));
        Check(Client->Identify(TEXT("user-one"), Person), TEXT("same-ID property update"));
        Check(!Client->Identify(TEXT("user-one"), Person), TEXT("duplicate property update suppressed"));
        Client->Group(TEXT("company"), TEXT("acme"));
        Check(Client->Capture(TEXT("stateful_identified")) == EPostHogCaptureResult::Accepted, TEXT("capture identified"));
        Check(Client->ResetAsync().Get(), TEXT("reset persistence"));
        const FString ResetId = Client->GetDistinctId();
        Check(ResetId != Anonymous && ResetId != TEXT("user-one"), TEXT("reset rotates identity"));
        Check(Client->GetSessionId() != FirstSession, TEXT("reset rotates session"));
        Check(Client->Capture(TEXT("stateful_reset")) == EPostHogCaptureResult::Accepted, TEXT("capture reset"));
        const auto Flush = Client->FlushAsync().Get();
        Check(Flush.bCompleted && Flush.Pending == 0 && Flush.StorageErrors == 0, TEXT("flush reports delivery"));
        Check(Client->ShutdownAsync().Get(), TEXT("shutdown"));
        Check(Client->ShutdownAsync().Get(), TEXT("idempotent shutdown"));
        Check(Client->Capture(TEXT("closed")) == EPostHogCaptureResult::Closed, TEXT("post-close capture"));
        Client.Reset();
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->GetDistinctId() == ResetId, TEXT("restore anonymous identity")); if (!Client) { return Result; }
        Check(Client->OptOutAsync().Get(), TEXT("persist and purge opt-out"));
        Check(Client->IsOptedOut(), TEXT("opted out"));
        Check(!Client->Identify(TEXT("ignored")), TEXT("opt-out identify guard"));
        Check(Client->Capture(TEXT("ignored")) == EPostHogCaptureResult::Disabled, TEXT("opt-out capture guard"));
        Check(Client->ShutdownAsync().Get(), TEXT("opted-out shutdown")); Client.Reset();
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->IsOptedOut(), TEXT("restore consent before admission")); if (!Client) { return Result; }
        Check(Client->ResetAsync().Get() && !Client->IsOptedOut(), TEXT("reset clears opt-out per spec"));
        Check(Client->Capture(TEXT("stateful_after_reset_optout")) == EPostHogCaptureResult::Accepted, TEXT("capture after reset consent"));
        Check(Client->FlushAsync().Get().Pending == 0, TEXT("flush reset consent"));
        Check(Client->ShutdownAsync().Get(), TEXT("shutdown reset consent")); Client.Reset();

        Config.ClientName = TEXT("hook");
        auto Weak = MakeShared<TWeakPtr<FPostHogClient, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>();
        Config.BeforeSend = [Weak](FPostHogEvent& Event)
        {
            Event.Properties.Remove(TEXT("secret")); Event.Properties.Remove(TEXT("$lib"));
            if (Event.Event == TEXT("hook_withdraw")) { if (auto Current = Weak->Pin()) { Current->OptOutAsync(); } }
            if (Event.Event == TEXT("hook_withdraw_resume"))
            { if (auto Current = Weak->Pin()) { Current->OptOutAsync().Get(); Current->OptInAsync().Get(); } }
            return true;
        };
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("hook client")); if (!Client) { return Result; } *Weak = Client;
        FPostHogProperties Secret; Secret.SetString(TEXT("secret"), TEXT("must not leave process"));
        Check(Client->Capture(TEXT("hook_withdraw"), Secret) == EPostHogCaptureResult::Disabled, TEXT("consent rechecked after hook"));
        Check(Client->OptInAsync().Get(), TEXT("opt back in"));
        Check(Client->Capture(TEXT("hook_withdraw_resume"), Secret) == EPostHogCaptureResult::Disabled, TEXT("opt-in does not revive pre-opt-out snapshots"));
        Check(Client->Capture(TEXT("stateful_redacted"), Secret) == EPostHogCaptureResult::Accepted, TEXT("capture redacted"));
        Check(Client->FlushAsync().Get().Pending == 0, TEXT("flush redacted"));
        Check(Client->ShutdownAsync().Get(), TEXT("close hook client")); Client.Reset();

        Config.BeforeSend = {}; Config.ClientName = TEXT("sessions");
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("session client")); if (!Client) { return Result; }
        const FString Session = Client->GetSessionId();
        *Now += FTimespan::FromMinutes(30);
        Check(Client->GetSessionId() == Session, TEXT("exact inactivity boundary"));
        *Now += FTimespan::FromMilliseconds(1);
        Check(Client->GetSessionId() != Session, TEXT("past inactivity boundary"));
        Client->OnBackground(); *Now += FTimespan::FromMinutes(31);
        Check(Client->GetSessionId().IsEmpty(), TEXT("background expired session is empty"));
        Client->OnForeground(); Check(!Client->GetSessionId().IsEmpty(), TEXT("foreground starts session"));
        const FString DaySession = Client->GetSessionId();
        for (int32 Index = 0; Index < 49; ++Index)
        {
            *Now += FTimespan::FromMinutes(29); Client->Capture(TEXT("session_activity"));
        }
        *Now += FTimespan::FromMinutes(19);
        Check(Client->GetSessionId() == DaySession, TEXT("exact maximum-duration boundary"));
        *Now += FTimespan::FromMilliseconds(1);
        Check(Client->GetSessionId() != DaySession, TEXT("past maximum-duration boundary"));
        Check(Client->ShutdownAsync().Get(), TEXT("close automatic sessions")); Client.Reset();

        Config.ClientName = TEXT("explicit"); Config.SessionMode = EPostHogSessionMode::Explicit;
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("explicit client")); if (!Client) { return Result; }
        Check(Client->GetSessionId().IsEmpty(), TEXT("explicit mode starts sessionless"));
        Client->Capture(TEXT("explicit_before"));
        Check(Client->StartSession(), TEXT("explicit start")); const FString Explicit = Client->GetSessionId();
        Check(Client->StartSession() && Client->GetSessionId() == Explicit, TEXT("explicit start idempotent"));
        Client->Capture(TEXT("explicit_during")); Client->EndSession();
        Check(Client->GetSessionId().IsEmpty(), TEXT("explicit end stays sessionless"));
        Client->Capture(TEXT("explicit_after"));
        Check(!Client->SetSessionId(TEXT("invalid")), TEXT("reject invalid session"));
        Check(Client->SetSessionId(Explicit), TEXT("accept compatible session"));
        Check(Client->FlushAsync().Get().Pending == 0, TEXT("flush explicit"));
        Check(Client->ShutdownAsync().Get(), TEXT("close explicit")); Client.Reset();
        Config.ClientName = TEXT("never"); Config.PersonProfiles = EPostHogPersonProfiles::Never;
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("personless client")); if (!Client) { return Result; }
        Check(!Client->Identify(TEXT("ignored")), TEXT("Never profile guard"));
        Client->Alias(TEXT("ignored")); Client->Group(TEXT("ignored"), TEXT("ignored")); Client->SetPersonProperties(Person);
        Client->Capture(TEXT("stateful_never"));
        Check(Client->FlushAsync().Get().Pending == 0, TEXT("flush personless capture"));
        Check(Client->ShutdownAsync().Get(), TEXT("close personless client")); Client.Reset();
        Config.PersonProfiles = EPostHogPersonProfiles::IdentifiedOnly;

        Config.ClientName = TEXT("bounds"); Config.SessionMode = EPostHogSessionMode::Automatic;
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("bounds client")); if (!Client) { return Result; }
        FPostHogProperties Large;
        Check(Large.SetString(TEXT("payload"), FString::ChrN(1024 * 1024 - 32, TEXT('x'))), TEXT("individual properties fit size bound"));
        Client->Register(Large);
        FPostHogProperties Deep;
        Check(FPostHogProperties::TryParse(TEXT("{\"payload\":") + FString::ChrN(63, TEXT('[')) + TEXT("0") + FString::ChrN(63, TEXT(']')) + TEXT("}"), Deep), TEXT("individual properties fit depth bound"));
        Client->Register(Deep);
        Check(Client->OptOutAsync().Get(), TEXT("oversized state rejected before consent persistence"));
        Check(Client->ShutdownAsync().Get(), TEXT("close bounds client")); Client.Reset();
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->IsOptedOut(), TEXT("size/depth boundary cannot erase opt-out")); if (!Client) { return Result; }
        auto OptIn = Client->OptInAsync();
        for (int32 Index = 0; Index < 50; ++Index)
        {
            FPostHogProperties Mutation; Mutation.SetNumber(TEXT("iteration"), Index); Client->Register(Mutation);
        }
        Check(OptIn.Get(), TEXT("opt-in with coalesced property writes"));
        const FString Namespace = FMD5::HashAnsiString(*(Config.Host + TEXT("\n") + Config.ProjectToken + TEXT("\n") + Config.ClientName));
        FString StateText; bool PersistedOptOut = true;
        Check(FFileHelper::LoadFileToString(StateText, *(Root / Namespace / TEXT("state.json"))), TEXT("read acknowledged consent"));
        auto SavedState = PostHogJson::Decode(StateText);
        Check(SavedState && SavedState->TryGetBoolField(TEXT("opted_out"), PersistedOptOut) && !PersistedOptOut, TEXT("successful opt-in acknowledges persisted consent"));
        Client->Capture(TEXT("stateful_bounds"));
        Check(Client->FlushAsync().Get().Pending == 0, TEXT("flush bounded state"));
        Check(Client->ShutdownAsync().Get(), TEXT("close bounded state")); Client.Reset();

        Config.ClientName = TEXT("purge-failure");
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("purge failure client")); if (!Client) { return Result; }
        const FString FailureNamespace = FMD5::HashAnsiString(*(Config.Host + TEXT("\n") + Config.ProjectToken + TEXT("\n") + Config.ClientName));
        const FString FailureDirectory = Root / FailureNamespace;
        const FString EventsDirectory = FailureDirectory / TEXT("events");
        const FString BackupDirectory = FailureDirectory / TEXT("events.saved");
        Client->Capture(TEXT("must_not_replay"));
        TArray<FString> DurableEvents;
        for (int32 Attempt = 0; Attempt < 200 && DurableEvents.IsEmpty(); ++Attempt)
        { IFileManager::Get().FindFiles(DurableEvents, *(EventsDirectory / TEXT("*.json")), true, false); FPlatformProcess::SleepNoStats(0.01f); }
        Check(!DurableEvents.IsEmpty(), TEXT("event durable before purge failure"));
        auto& Files = FPlatformFileManager::Get().GetPlatformFile();
        Check(Files.MoveFile(*BackupDirectory, *EventsDirectory), TEXT("retain old spool for failure injection"));
        Check(FFileHelper::SaveStringToFile(TEXT("not a directory"), *EventsDirectory), TEXT("inject purge failure"));
        Check(!Client->OptOutAsync().Get(), TEXT("purge failure reported"));
        Check(!Client->OptInAsync().Get() && Client->IsOptedOut(), TEXT("failed purge cannot grant consent"));
        Check(FFileHelper::LoadFileToString(StateText, *(FailureDirectory / TEXT("state.json"))), TEXT("read failed opt-in consent"));
        SavedState = PostHogJson::Decode(StateText); PersistedOptOut = false;
        Check(SavedState && SavedState->TryGetBoolField(TEXT("opted_out"), PersistedOptOut) && PersistedOptOut, TEXT("failed purge preserves persisted opt-out"));
        Check(Client->ShutdownAsync().Get(), TEXT("close failed purge client")); Client.Reset();
        Check(Files.DeleteFile(*EventsDirectory) && Files.MoveFile(*EventsDirectory, *BackupDirectory), TEXT("restore original spool"));
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->IsOptedOut(), TEXT("restart remains opted out with old spool"));
        if (Client) { Check(Client->ShutdownAsync().Get(), TEXT("close restored failure client")); Client.Reset(); }

        Config.ClientName = TEXT("parallel-a"); auto FirstFuture = FPostHogClient::CreateAsync(Config);
        Config.ClientName = TEXT("parallel-b"); auto SecondFuture = FPostHogClient::CreateAsync(Config);
        auto First = FirstFuture.Get(); auto Second = SecondFuture.Get();
        Check(First && Second, TEXT("parallel clients"));
        if (First && Second)
        {
            Check(First->GetDistinctId() != Second->GetDistinctId(), TEXT("separate namespaces isolate identity"));
            auto A = Async(EAsyncExecution::Thread, [First] { for (int32 Index = 0; Index < 25; ++Index) { First->Capture(TEXT("parallel_a")); } });
            auto B = Async(EAsyncExecution::Thread, [Second] { for (int32 Index = 0; Index < 25; ++Index) { Second->Capture(TEXT("parallel_b")); } });
            A.Wait(); B.Wait();
            Check(First->FlushAsync().Get().Pending == 0 && Second->FlushAsync().Get().Pending == 0, TEXT("parallel delivery"));
            auto CloseA = First->ShutdownAsync(); auto CloseB = Second->ShutdownAsync();
            Check(CloseA.Get() && CloseB.Get(), TEXT("parallel teardown"));
        }
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return Result;
    });
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FWaitForClientTests>(this, MoveTemp(Future)));
    return true;
}
#endif
