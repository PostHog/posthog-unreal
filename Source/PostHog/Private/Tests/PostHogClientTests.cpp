#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogTestHelpers.h"
#include "PostHogJson.h"
#include "PostHogNative.h"
#include "PostHogReplayRecorder.h"
#include "CoreGlobals.h"
#include "UObject/Class.h"
#include <limits>
#include <atomic>

struct FPostHogHostCloseTestAccess
{
    static bool Bind(FPostHogClient& Client, TFunction<void()>&& Cancel, TSharedFuture<bool> Done)
    { return Client.BindHostClose(MoveTemp(Cancel), [Done]() -> FPostHogClient::FHostClose
        { return {Done, FPostHogReplayRecorder::ShouldDeferRetirement(Done)}; }); }
};

namespace
{
FPostHogProperties LargeFlagRecord()
{
    auto Object = PostHogJson::Decode(TEXT("{\"enabled\":true,\"has_payload\":true,\"unrelated\":18446744073709551615,\"remove_me\":true}"));
    const FString Large = FString::ChrN(700 * 1024, TEXT('x'));
    Object->SetStringField(TEXT("variant"), Large); Object->SetStringField(TEXT("payload"), Large);
    return PostHogNative::Flag(MakeShared<FJsonValueObject>(Object)).Record;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogPropertiesTest, "PostHog.Properties", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogPropertiesTest::RunTest(const FString&)
{
    FPostHogProperties Properties;
    TestTrue(TEXT("UTF-8 properties"), Properties.SetString(TEXT("text"), TEXT("玩家 🚀")));
    TestTrue(TEXT("finite numbers"), Properties.SetNumber(TEXT("number"), 42.5));
    TestTrue(TEXT("false"), Properties.SetBoolean(TEXT("boolean"), false));
    auto Copy = Properties; Copy.SetString(TEXT("text"), TEXT("copy"));
    TestEqual(TEXT("owned copies"), Properties.ToNative()->GetStringField(TEXT("text")), FString(TEXT("玩家 🚀")));
    FPostHogValue Value;
    TestTrue(TEXT("explicit null"), FPostHogValue::TryParse(TEXT("null"), Value) && Value.ToNative()->IsNull());
    TestFalse(TEXT("malformed JSON"), FPostHogValue::TryParse(TEXT("{bad"), Value));
    TestFalse(TEXT("multiple roots"), FPostHogValue::TryParse(TEXT("1,2"), Value));
    TestFalse(TEXT("non-object properties"), FPostHogProperties::TryParse(TEXT("[]"), Copy));
    TestFalse(TEXT("NaN"), Properties.SetNumber(TEXT("bad"), std::numeric_limits<double>::quiet_NaN()));
    TestFalse(TEXT("depth bound"), FPostHogValue::TryParse(FString::ChrN(129, TEXT('[')) + TEXT("0") + FString::ChrN(129, TEXT(']')), Value));
    TestFalse(TEXT("input size bound"), Properties.SetString(TEXT("big"), FString::ChrN(1024 * 1024, TEXT('x'))));
    const FString Exact = TEXT("{\"integer\":18446744073709551615,\"decimal\":1.234567890123456789}");
    const auto Object = PostHogJson::Decode(Exact);
    TestTrue(TEXT("native numeric lexemes preserved"), Object && PostHogJson::Encode(Object.ToSharedRef()).Contains(TEXT("18446744073709551615")) &&
        PostHogJson::Encode(Object.ToSharedRef()).Contains(TEXT("1.234567890123456789")));
    const auto SDKRecord = PostHogJson::Decode(TEXT(R"({"enabled":false,"variant":null,"has_payload":true,"payload":{"paired":"good"},"request_id":"source","evaluated_at":123,"context":{"distinct_id":"person"}})"));
    const auto HealthyFalse = PostHogNative::Flag(MakeShared<FJsonValueObject>(SDKRecord));
    TestTrue(TEXT("SDK healthy false remains a found result"), HealthyFalse.bFound && !HealthyFalse.bEnabled);
    TestTrue(TEXT("SDK paired payload is retained"), HealthyFalse.bHasPayload && HealthyFalse.Payload.ToJson() == TEXT(R"({"paired":"good"})"));
    TestTrue(TEXT("SDK producing metadata/context retained without recomputation"), HealthyFalse.Record.ToNative()->GetStringField(TEXT("request_id")) == TEXT("source") &&
        HealthyFalse.Record.ToNative()->GetObjectField(TEXT("context"))->GetStringField(TEXT("distinct_id")) == TEXT("person"));
    TestFalse(TEXT("SDK missing result stays distinct from healthy false"), PostHogNative::Flag(MakeShared<FJsonValueNull>()).bFound);
    auto Record = LargeFlagRecord(); const FString Original = Record.ToJson();
    TestTrue(TEXT("large output is accepted and nonempty"), Record.IsValid() && !Record.IsEmpty() && FTCHARToUTF8(*Original).Length() > PHU_MAX_INPUT);
    TestEqual(TEXT("large output ToNative retains payload"), Record.ToNative()->GetStringField(TEXT("payload")).Len(), 700 * 1024);
    TestTrue(TEXT("large output retains unrelated exact number"), PostHogJson::Encode(Record.ToNative()).Contains(TEXT("18446744073709551615")));
    auto LargeCopy = Record; LargeCopy.Remove(TEXT("remove_me"));
    TestTrue(TEXT("Remove preserves large unrelated fields"), !LargeCopy.ToNative()->HasField(TEXT("remove_me")) &&
        LargeCopy.ToNative()->GetStringField(TEXT("payload")).Len() == 700 * 1024 && LargeCopy.ToNative()->HasField(TEXT("unrelated")));
    TestTrue(TEXT("Set retains accepted output bound"), LargeCopy.SetString(TEXT("added"), TEXT("value")));
    TestTrue(TEXT("Set preserves original large fields"), LargeCopy.ToNative()->GetStringField(TEXT("variant")).Len() == 700 * 1024 &&
        LargeCopy.ToNative()->GetStringField(TEXT("added")) == TEXT("value") && LargeCopy.ToNative()->HasField(TEXT("unrelated")));
    TestEqual(TEXT("mutating copy does not change source"), Record.ToJson(), Original);
    auto Shrunk = Record; Shrunk.Remove(TEXT("variant"));
    TestTrue(TEXT("removing a large field shrinks below input bound"), FTCHARToUTF8(*Shrunk.ToJson()).Length() < PHU_MAX_INPUT);
    TestTrue(TEXT("shrinking does not lower accepted mutation bound"), Shrunk.SetString(TEXT("variant"), Record.ToNative()->GetStringField(TEXT("variant"))) && !Shrunk.IsEmpty());
    FPostHogProperties ReflectedCopy; FPostHogProperties::StaticStruct()->CopyScriptStruct(&ReflectedCopy, &Record);
    TestTrue(TEXT("reflected copy retains output bound"), !ReflectedCopy.IsEmpty() && ReflectedCopy.SetBoolean(TEXT("copied"), true) &&
        ReflectedCopy.ToNative()->GetStringField(TEXT("payload")).Len() == 700 * 1024);
    FPostHogProperties Parsed;
    TestTrue(TEXT("explicit large parse keeps accepted bound"), FPostHogProperties::TryParse(Original, Parsed, PHU_MAX_OUTPUT) &&
        !Parsed.IsEmpty() && Parsed.SetBoolean(TEXT("added"), true) && Parsed.ToNative()->HasField(TEXT("payload")));
    const FString BeforeOverflow = LargeCopy.ToJson();
    TestFalse(TEXT("output mutation cannot exceed accepted bound"), LargeCopy.SetString(TEXT("overflow"), FString::ChrN(PHU_MAX_OUTPUT, TEXT('x'))));
    TestEqual(TEXT("failed mutation retains complete record"), LargeCopy.ToJson(), BeforeOverflow);
    TestFalse(TEXT("default input parse remains bounded"), FPostHogProperties::TryParse(Original, Parsed));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientTest, "PostHog.StatefulClient", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogClientTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors; auto Check = [&Errors](bool Pass, const TCHAR* Message) { if (!Pass) { Errors.Add(Message); } };
        auto Weak = MakeShared<TWeakPtr<FPostHogClient, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>();
        auto Config = PostHogTests::Config(TEXT("adapter-client"));
        Config.BeforeSend = [Weak, &Check](FJsonObject& Event)
        {
            Check(Event.HasTypedField<EJson::Object>(TEXT("options")) && Event.HasTypedField<EJson::Object>(TEXT("groups")) &&
                Event.HasTypedField<EJson::String>(TEXT("uuid")) && Event.HasTypedField<EJson::String>(TEXT("timestamp")), TEXT("complete native hook event"));
            const FString Name = Event.GetStringField(TEXT("event"));
            auto Current = Weak->Pin();
            if (Name == TEXT("drop")) { return false; }
#if !PLATFORM_EXCEPTIONS_DISABLED
            if (Name == TEXT("throw")) { throw 1; }
#endif
            if (Name == TEXT("recursive") && Current) { Check(Current->Capture(TEXT("nested")) == EPostHogResult::Reentrant, TEXT("core rejects recursive capture")); }
            if (Name == TEXT("close-in-hook") && Current) { Check(Current->ShutdownAsync().Get().Status == EPostHogResult::Deferred, TEXT("callback close cannot self-wait")); }
            if (Name == TEXT("flush-in-hook") && Current) { Check(Current->FlushAsync().Get().Delivery == EPostHogDeliveryOutcome::Deferred, TEXT("callback flush stays on callback thread")); }
            if (Name == TEXT("adapter_client")) { Event.GetObjectField(TEXT("properties"))->RemoveField(TEXT("secret")); }
            return true;
        };
        auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get(); Check(Client.IsValid(), TEXT("create native adapter")); if (!Client) { return Errors; }
        *Weak = Client;
        const auto Large = LargeFlagRecord();
        Check(Large.IsValid() && !Large.IsEmpty(), TEXT("accepted large output fixture"));
        Check(Client->Register(Large) == EPostHogResult::InvalidInput, TEXT("command input bound rejects accepted large output"));
        Check(Client->Capture(TEXT("oversized-arguments"), Large) == EPostHogResult::InvalidInput, TEXT("capture input bound rejects accepted large output"));
        Check(Client->Screen(TEXT("oversized-screen"), Large) == EPostHogResult::InvalidInput, TEXT("screen input bound rejects accepted large output"));
        Check(Client->IsInitialized() && !Client->IsDisabled(), TEXT("initialization separate from enablement"));
        FPostHogProperties Properties, Options; Properties.SetString(TEXT("secret"), TEXT("redact")); Options.SetBoolean(TEXT("process_person_profile"), true);
        Check(Client->Capture(TEXT("adapter_client"), Properties, Options) == EPostHogResult::Accepted, TEXT("capture delegates"));
        Check(Client->Identify(TEXT("adapter-user")) == EPostHogResult::Ok, TEXT("identify invocation"));
        Check(Client->Identify(TEXT("adapter-user")) == EPostHogResult::Ok, TEXT("duplicate does not invent a rejection result"));
        Check(Client->Capture(TEXT("drop")) == EPostHogResult::Dropped, TEXT("hook drop"));
#if !PLATFORM_EXCEPTIONS_DISABLED
        Check(Client->Capture(TEXT("throw")) == EPostHogResult::Dropped, TEXT("foreign exceptions fail closed before the ABI"));
#endif
        Check(Client->Capture(TEXT("recursive")) == EPostHogResult::Accepted, TEXT("outer capture"));
        Check(Client->Capture(TEXT("flush-in-hook")) == EPostHogResult::Accepted, TEXT("flush callback returns"));
        Client->OptOut(); Check(Client->Capture(TEXT("blocked")) == EPostHogResult::OptedOut, TEXT("Rust consent outcome")); Client->OptIn();
        const auto Drain = Client->FlushAsync().Get(); Check(Drain.Status == EPostHogResult::Ok && Drain.State == EPostHogPersistenceOutcome::MemoryOnly, TEXT("real independent drain outcomes"));
        Client->Capture(TEXT("close-in-hook"));
        Check(Client->ShutdownAsync().Get().bJoined, TEXT("external close joins after hook"));
        Check(Client->ShutdownAsync().Get().bJoined && Client->Capture(TEXT("after-close")) == EPostHogResult::Closed, TEXT("completed close remains completed"));
        Client.Reset();
        return Errors;
    });
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogNestedHookTest, "PostHog.NestedHookClose", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogNestedHookTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors;
        auto AWeak = MakeShared<TWeakPtr<FPostHogClient, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>();
        auto BWeak = MakeShared<TWeakPtr<FPostHogClient, ESPMode::ThreadSafe>, ESPMode::ThreadSafe>();
        auto AConfig = PostHogTests::Config(TEXT("nested-a")); auto BConfig = PostHogTests::Config(TEXT("nested-b"));
        AConfig.BeforeSend = [BWeak](FJsonObject&) { if (auto B = BWeak->Pin()) { B->Capture(TEXT("nested-b")); } return true; };
        BConfig.BeforeSend = [AWeak, &Errors](FJsonObject&)
        {
            if (auto A = AWeak->Pin())
            { if (A->ShutdownAsync().Get().Status != EPostHogResult::Deferred) { Errors.Add(TEXT("nested callback must not wait for its outer active call")); } }
            return true;
        };
        auto A = FPostHogClient::CreateAsync(MoveTemp(AConfig)).Get(); auto B = FPostHogClient::CreateAsync(MoveTemp(BConfig)).Get();
        if (!A || !B) { if (A) { A->ShutdownAsync().Get(); } if (B) { B->ShutdownAsync().Get(); } Errors.Add(TEXT("nested callback clients")); return Errors; }
        *AWeak = A; *BWeak = B;
        A->Capture(TEXT("nested-a"));
        if (!A->ShutdownAsync().Get().bJoined || !B->ShutdownAsync().Get().bJoined) { Errors.Add(TEXT("nested callback clients join")); }
        return Errors;
    });
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogHostCloseTest, "PostHog.HostRetirement", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogHostCloseTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors;
        auto Client = FPostHogClient::CreateAsync(PostHogTests::Config(TEXT("host-retirement"))).Get();
        if (!Client) { Errors.Add(TEXT("create")); return Errors; }
        auto Cancelled = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
        TPromise<bool> Done; const auto Host = Done.GetFuture().Share();
        if (!FPostHogHostCloseTestAccess::Bind(*Client, [Cancelled] { Cancelled->store(true); }, Host))
        { Errors.Add(TEXT("bind retirement")); Done.SetValue(true); Client->ShutdownAsync().Get(); return Errors; }
        if (IsInAnyRenderingThread() || FPostHogReplayRecorder::ShouldDeferRetirement(Host))
        { Errors.Add(TEXT("ordinary FRunnable pending retirement is not a render task")); }
        const auto Close = Client->ShutdownAsync();
        if (!Cancelled->load() || Close.IsReady()) { Errors.Add(TEXT("close must cancel first and wait for retirement")); }
        if (Client->Capture(TEXT("after-close")) != EPostHogResult::Closed) { Errors.Add(TEXT("admission closed during host retirement")); }
        Done.SetValue(true);
        if (!Close.Get().bJoined) { Errors.Add(TEXT("one operation completes after host and native join")); }
        if (FPostHogReplayRecorder::ShouldDeferRetirement(Host)) { Errors.Add(TEXT("completed retirement is not deferred on ordinary worker")); }
        TPromise<bool> PendingPromise; const auto Pending = PendingPromise.GetFuture().Share();
        for (ETaskTag Role : {ETaskTag::EParallelRenderingThread, ETaskTag::EParallelRhiThread})
        {
            // Explicit render-role test scope; production FRunnable threads remain untagged.
            FTaskTagScope RenderRole(Role);
            if (!FPostHogReplayRecorder::ShouldDeferRetirement(Pending)) { Errors.Add(TEXT("pending rendering/RHI task retirement must defer")); }
            if (FPostHogReplayRecorder::ShouldDeferRetirement(Host)) { Errors.Add(TEXT("completed rendering/RHI task retirement must not defer")); }
        }
        PendingPromise.SetValue(true);
        if (IsInAnyRenderingThread() || FPostHogReplayRecorder::ShouldDeferRetirement(Pending))
        { Errors.Add(TEXT("render-role scope restores ordinary worker")); }
        return Errors;
    });
}
#endif
