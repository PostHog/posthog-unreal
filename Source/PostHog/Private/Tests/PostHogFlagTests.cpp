#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include "PostHogFlagCache.h"
#include "PostHogJson.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFlagCacheTest, "PostHog.FlagCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogFlagCacheTest::RunTest(const FString& Parameters)
{
    FPostHogFlagCache Cache;
    auto Response = PostHogJson::Decode(TEXT(R"({"flags":{
        "off":{"value":false,"payload":null,"has_experiment":false,"minimal_flag_called_events":true},
        "variant":{"value":"blue","payload":"true","id":12,"version":4,"has_experiment":true,"minimal_flag_called_events":true},
        "object":{"value":true,"payload":{"color":"green"}},
        "array":{"value":true,"payload":[false,0,null]},
        "zero":{"value":true,"payload":0},"false":{"value":true,"payload":false},
        "empty":{"value":true,"payload":""},"absent":{"value":true}
        },"request_id":"first","evaluated_at":123,"errors_while_computing":false,"quota_limited":false})"));
    TestEqual(TEXT("complete response"), Cache.Apply(Response.ToSharedRef()), EPostHogFlagReloadStatus::Success);
    TestTrue(TEXT("known false differs from missing"), Cache.Flags.Contains(TEXT("off")) && !Cache.Flags[TEXT("off")].Value.ToNative()->AsBool() && !Cache.Flags.Contains(TEXT("missing")));
    TestTrue(TEXT("null is a present payload"), Cache.Flags[TEXT("off")].bHasPayload && Cache.Flags[TEXT("off")].Payload.ToNative()->IsNull());
    TestFalse(TEXT("absent payload"), Cache.Flags[TEXT("absent")].bHasPayload);
    TestEqual(TEXT("decoded strings are not parsed twice"), Cache.Flags[TEXT("variant")].Payload.ToJson(), FString(TEXT("\"true\"")));
    TestEqual(TEXT("empty string payload"), Cache.Flags[TEXT("empty")].Payload.ToJson(), FString(TEXT("\"\"")));
    TestEqual(TEXT("false payload"), Cache.Flags[TEXT("false")].Payload.ToJson(), FString(TEXT("false")));
    TestEqual(TEXT("zero payload"), Cache.Flags[TEXT("zero")].Payload.ToNative()->AsNumber(), 0.0);
    TestEqual(TEXT("array payload"), Cache.Flags[TEXT("array")].Payload.ToNative()->AsArray().Num(), 3);
    TestEqual(TEXT("object payload"), Cache.Flags[TEXT("object")].Payload.ToNative()->AsObject()->GetStringField(TEXT("color")), FString(TEXT("green")));
    TestTrue(TEXT("server gate and explicit non-experiment"), FPostHogFlagCache::IsMinimal(Cache.Flags[TEXT("off")]));
    TestFalse(TEXT("experiment stays full"), FPostHogFlagCache::IsMinimal(Cache.Flags[TEXT("variant")]));
    TestFalse(TEXT("unknown linkage stays full"), FPostHogFlagCache::IsMinimal(Cache.Flags[TEXT("object")]));
    auto Exposure = FPostHogFlagCache::Exposure(TEXT("variant"), Cache.Flags[TEXT("variant")]);
    TestEqual(TEXT("evaluation metadata"), Exposure.ToNative()->GetNumberField(TEXT("$feature_flag_id")), 12.0);
    Exposure.SetString(TEXT("private"), TEXT("drop")); Exposure.SetString(TEXT("$referrer"), TEXT("drop"));
    Exposure.SetString(TEXT("utm_source"), TEXT("keep")); Exposure.SetString(TEXT("$lib"), TEXT("posthog-unreal"));
    FPostHogFlagCache::Minimize(Exposure);
    TestFalse(TEXT("minimal removes custom/referrer"), Exposure.ToNative()->HasField(TEXT("private")) || Exposure.ToNative()->HasField(TEXT("$referrer")));
    TestTrue(TEXT("minimal keeps attribution and SDK"), Exposure.ToNative()->HasField(TEXT("utm_source")) && Exposure.ToNative()->HasField(TEXT("$lib")));
    Cache.Reported.Add(TEXT("variant"));
    auto Partial = PostHogJson::Decode(TEXT(R"({"flags":{"off":{"value":true}},"request_id":"second","errors_while_computing":true,"quota_limited":false})"));
    TestEqual(TEXT("partial response"), Cache.Apply(Partial.ToSharedRef()), EPostHogFlagReloadStatus::Partial);
    TestEqual(TEXT("retained flag keeps original request metadata"), Cache.Flags[TEXT("variant")].Metadata.ToNative()->GetStringField(TEXT("request_id")), FString(TEXT("first")));
    TestEqual(TEXT("new flag gets new request metadata"), Cache.Flags[TEXT("off")].Metadata.ToNative()->GetStringField(TEXT("request_id")), FString(TEXT("second")));
    TestTrue(TEXT("reload clears dedup"), Cache.Reported.IsEmpty());
    FPostHogFlagCache Restored;
    TestTrue(TEXT("cache round trip"), Restored.Restore(Cache.Serialize()));
    TestEqual(TEXT("payload retained"), Restored.Flags[TEXT("variant")].Payload.ToJson(), FString(TEXT("\"true\"")));
    TestTrue(TEXT("minimal gate retained"), Restored.Flags[TEXT("variant")].Metadata.ToNative()->GetBoolField(TEXT("minimal_flag_called_events")));
    auto Quota = PostHogJson::Decode(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":true})"));
    TestEqual(TEXT("quota response"), Cache.Apply(Quota.ToSharedRef()), EPostHogFlagReloadStatus::QuotaLimited);
    TestTrue(TEXT("quota retains cache"), Cache.Flags.Contains(TEXT("variant")));
    auto Empty = PostHogJson::Decode(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":false})"));
    TestEqual(TEXT("empty success"), Cache.Apply(Empty.ToSharedRef()), EPostHogFlagReloadStatus::Success);
    TestTrue(TEXT("empty replaces old values"), Cache.bLoaded && Cache.Flags.IsEmpty());
    Cache.Clear(); TestFalse(TEXT("clear unloads cache"), Cache.bLoaded);
    return true;
}

namespace
{
struct FFlagTestResult { TArray<FString> Errors; };
class FWaitForFlagTests final : public IAutomationLatentCommand
{
public:
    FWaitForFlagTests(FAutomationTestBase* InTest, TFuture<FFlagTestResult>&& InFuture) : Test(InTest), Future(MoveTemp(InFuture)) {}
    bool Update() override
    {
        if (!Future.IsReady()) { return false; }
        for (const auto& Error : Future.Get().Errors) { Test->AddError(Error); }
        return true;
    }
private:
    FAutomationTestBase* Test;
    TFuture<FFlagTestResult> Future;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFlagsTest, "PostHog.Flags",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogFlagsTest::RunTest(const FString& Parameters)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    const FString ControlRoot = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_CONTROL_DIRECTORY"));
    if (Host.IsEmpty() || ControlRoot.IsEmpty()) { AddError(TEXT("Run with Tests/native_integration.py for mock-server controls.")); return false; }
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogFlagTests") / FGuid::NewGuid().ToString());
    auto Future = Async(EAsyncExecution::Thread, [Host, ControlRoot, Root]
    {
        FFlagTestResult Result;
        auto Check = [&Result](bool Pass, const TCHAR* Message) { if (!Pass) { Result.Errors.Add(Message); } };
        int32 Command = 0;
        // The Python driver owns the maintained mock server and executes controls.
        // Files keep test HTTP dependencies out of the runtime module.
        auto Control = [&](const FString& Path, const FString& Body = FString()) -> TSharedRef<FJsonObject>
        {
            const int32 Id = ++Command;
            auto Request = MakeShared<FJsonObject>(); Request->SetStringField(TEXT("path"), Path);
            if (!Body.IsEmpty()) { Request->SetObjectField(TEXT("body"), PostHogJson::Decode(Body)); }
            const FString RequestPath = ControlRoot / FString::Printf(TEXT("request-%d.json"), Id);
            FFileHelper::SaveStringToFile(PostHogJson::Encode(Request), *(RequestPath + TEXT(".tmp")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
            IFileManager::Get().Move(*RequestPath, *(RequestPath + TEXT(".tmp")));
            const FString ResponsePath = ControlRoot / FString::Printf(TEXT("response-%d.json"), Id);
            for (int32 Attempt = 0; Attempt < 1000; ++Attempt)
            {
                if (IFileManager::Get().FileExists(*ResponsePath))
                {
                    FString Text; FFileHelper::LoadFileToString(Text, *ResponsePath);
                    auto Response = PostHogJson::Decode(Text);
                    const TSharedPtr<FJsonObject>* Data = nullptr;
                    if (Response && Response->TryGetObjectField(TEXT("data"), Data)) { return Data->ToSharedRef(); }
                    break;
                }
                FPlatformProcess::SleepNoStats(0.01f);
            }
            Check(false, TEXT("mock control failed")); return MakeShared<FJsonObject>();
        };
        auto CallCount = [&]() { auto Data = Control(TEXT("/__captured/all")); const TArray<TSharedPtr<FJsonValue>>* Calls = nullptr;
            return Data->TryGetArrayField(TEXT("flags_calls"), Calls) ? Calls->Num() : -1; };
        FPostHogConfig Config; Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = Host; Config.StorageRoot = Root;
        Config.bPreloadFeatureFlags = false; Config.FlushAt = 100; Config.FlushIntervalMilliseconds = 60000;
        auto Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("create flags client")); if (!Client) { return Result; }
        FPostHogProperties Tags; Tags.SetString(TEXT("test_suite"), TEXT("flags")); Tags.SetString(TEXT("stage"), TEXT("initial")); Client->Register(Tags);
        Check(!Client->HasLoadedFeatureFlags() && !Client->GetFeatureFlagResult(TEXT("missing")).bFound, TEXT("cold cache is missing"));
        Check(Client->IsFeatureEnabled(TEXT("missing"), false, true) && !Client->IsFeatureEnabled(TEXT("missing"), false, false), TEXT("cold-cache caller defaults"));
        const int32 Before = CallCount();
        auto Loaded = Client->ReloadFeatureFlagsAsync().Get();
        Check(Loaded.Status == EPostHogFlagReloadStatus::Success && Loaded.bPersisted, TEXT("load and persist flags"));
        Check(Loaded.Flags.ToNative()->GetBoolField(TEXT("bool-value")) && !Loaded.Flags.ToNative()->GetBoolField(TEXT("disabled-flag")), TEXT("completion carries true and false values"));
        Check(Client->IsFeatureEnabled(TEXT("missing"), false, true) && !Client->IsFeatureEnabled(TEXT("disabled-flag"), false, true), TEXT("missing defaults and known-false precedence"));
        Check(Client->GetFeatureFlagResult(TEXT("disabled-flag"), false).bFound && !Client->IsFeatureEnabled(TEXT("disabled-flag")), TEXT("known false"));
        Check(!Client->IsFeatureEnabled(TEXT("disabled-flag")), TEXT("dedup false exposure"));
        Check(Client->GetFeatureFlagPayload(TEXT("string-value")).ToJson() == TEXT("\"test\""), TEXT("decoded string payload"));
        Client->GetFeatureFlag(TEXT("string-value"), false);
        Check(Client->IsFeatureEnabled(TEXT("string-value")), TEXT("variant enabled"));
        TArray<TFuture<void>> Readers;
        for (int32 Index = 0; Index < 8; ++Index)
        { Readers.Add(Async(EAsyncExecution::Thread, [Client] { Client->GetFeatureFlag(TEXT("bool-value")); })); }
        for (auto& Reader : Readers) { Reader.Wait(); }
        Client->GetFeatureFlags(); Client->GetFeatureFlagResult(TEXT("missing"));
        Check(CallCount() == Before + 1, TEXT("all getters are local"));
        Client->Capture(TEXT("flags_enriched")); Client->FlushAsync().Get();

        Control(TEXT("/__control/state"), TEXT(R"({"force500":{"flags":true}})"));
        const auto Failure = Client->ReloadFeatureFlagsAsync().Get();
        Check(Failure.Status == EPostHogFlagReloadStatus::Failed && Failure.Flags.ToNative()->GetBoolField(TEXT("bool-value")), TEXT("network failure carries last-known values"));
        Check(Client->IsFeatureEnabled(TEXT("bool-value"), false), TEXT("same-context failure keeps cache"));
        Control(TEXT("/__control/state"), TEXT(R"({"force500":{"flags":false},"flagsQuotaLimited":true})"));
        const auto QuotaStatus = Client->ReloadFeatureFlagsAsync().Get().Status;
        Check(QuotaStatus == EPostHogFlagReloadStatus::QuotaLimited || QuotaStatus == EPostHogFlagReloadStatus::Failed, TEXT("quota error surfaced"));
        Check(Client->IsFeatureEnabled(TEXT("bool-value"), false), TEXT("quota keeps cache"));
        Control(TEXT("/__control/state"), TEXT(R"({"flagsQuotaLimited":false})"));
        const FString Identity = Client->GetDistinctId();
        Check(Client->ShutdownAsync().Get(), TEXT("close flags client")); Client.Reset();
        const FString Namespace = FMD5::HashAnsiString(*(Config.Host + TEXT("\n") + Config.ProjectToken + TEXT("\n") + Config.ClientName));
        const FString StatePath = Root / Namespace / TEXT("state.json");
        FString Text; FFileHelper::LoadFileToString(Text, *StatePath);
        auto Saved = PostHogJson::Decode(Text);
        auto Cached = Saved->GetObjectField(TEXT("cached_flags"));
        auto Rich = PostHogJson::Decode(TEXT(R"({"value":true,"payload":{"color":"green"},"id":9,"version":2,"request_id":"rich-request","has_experiment":false,"minimal_flag_called_events":true})"));
        Cached->SetObjectField(TEXT("minimal"), Rich);
        auto Experiment = PostHogJson::Decode(PostHogJson::Encode(Rich.ToSharedRef()));
        Experiment->SetBoolField(TEXT("has_experiment"), true); Cached->SetObjectField(TEXT("experiment"), Experiment);
        auto Unknown = PostHogJson::Decode(PostHogJson::Encode(Rich.ToSharedRef()));
        Unknown->RemoveField(TEXT("has_experiment")); Cached->SetObjectField(TEXT("unknown"), Unknown);
        auto Ungated = PostHogJson::Decode(PostHogJson::Encode(Rich.ToSharedRef()));
        Ungated->SetBoolField(TEXT("minimal_flag_called_events"), false); Cached->SetObjectField(TEXT("ungated"), Ungated);
        const FString MatchingState = PostHogJson::Encode(Saved.ToSharedRef());
        Saved->GetObjectField(TEXT("flag_context"))->SetStringField(TEXT("distinct_id"), TEXT("another-context"));
        FFileHelper::SaveStringToFile(PostHogJson::Encode(Saved.ToSharedRef()), *StatePath);
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && !Client->HasLoadedFeatureFlags(), TEXT("mismatched persisted context is rejected"));
        if (!Client) { return Result; }
        Client->ShutdownAsync().Get(); Client.Reset();
        FFileHelper::SaveStringToFile(MatchingState, *StatePath);
        Config.BeforeSend = [](FPostHogEvent& Event)
        {
            Event.Properties.SetString(TEXT("hook_private"), TEXT("custom"));
            Event.Properties.Remove(TEXT("$lib_version")); return true;
        };
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->GetDistinctId() == Identity && Client->HasLoadedFeatureFlags(), TEXT("context-matched cache restored")); if (!Client) { return Result; }
        Tags.SetString(TEXT("stage"), TEXT("restored")); Tags.SetString(TEXT("utm_source"), TEXT("campaign"));
        Tags.SetString(TEXT("$referring_domain"), TEXT("example.com")); Tags.SetString(TEXT("$referrer"), TEXT("https://example.com/private")); Client->Register(Tags);
        Check(Client->IsFeatureEnabled(TEXT("bool-value")), TEXT("fresh tracker after restart"));
        for (const FString& Key : {FString(TEXT("minimal")), FString(TEXT("experiment")), FString(TEXT("unknown")), FString(TEXT("ungated"))})
        { Check(Client->IsFeatureEnabled(Key), TEXT("restored rich flag")); }
        Client->FlushAsync().Get();

        Control(TEXT("/__control/state"), TEXT(R"({"delays":{"flags":0.2}})"));
        const int32 BeforeCoalescing = CallCount();
        auto Loading = Client->ReloadFeatureFlagsAsync();
        TArray<TSharedFuture<FPostHogFlagReloadResult>> Loads;
        for (int32 Index = 0; Index < 8; ++Index) { Loads.Add(Client->ReloadFeatureFlagsAsync()); }
        for (auto& Load : Loads) { Check(Load.Get().Status == EPostHogFlagReloadStatus::Success, TEXT("coalesced completion")); }
        Check(CallCount() == BeforeCoalescing + 1, TEXT("concurrent reloads issue one request"));
        auto OldContext = Client->ReloadFeatureFlagsAsync();
        Client->Identify(TEXT("flag-user"));
        Check(Client->HasLoadedFeatureFlags() && Client->IsFeatureEnabled(TEXT("bool-value"), false), TEXT("identify keeps previous flags readable during reload"));
        Check(OldContext.Get().Status == EPostHogFlagReloadStatus::Success, TEXT("cycle follows newest context"));
        auto Calls = Control(TEXT("/__captured/all")); const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
        Check(Calls->TryGetArrayField(TEXT("flags_calls"), Entries) && Entries->Num() > 0 && Entries->Last()->AsObject()->GetStringField(TEXT("distinct_id")) == TEXT("flag-user"), TEXT("latest request uses current identity"));
        FPostHogProperties Person; Person.SetString(TEXT("plan"), TEXT("pro"));
        auto PersistedWithoutCache = [&]
        {
            Client->FlushAsync().Get(); FString StateText; FFileHelper::LoadFileToString(StateText, *StatePath);
            auto State = PostHogJson::Decode(StateText);
            return State && !State->HasField(TEXT("cached_flags")) && !State->HasField(TEXT("flag_context"));
        };
        Check(Client->SetPersonPropertiesForFlags(Person, false) && Client->HasLoadedFeatureFlags(), TEXT("targeting keeps flags readable without reload"));
        Check(PersistedWithoutCache(), TEXT("stale cache is not persisted under the new context"));
        const int32 BeforePerson = CallCount();
        Check(Client->SetPersonPropertiesForFlags(Person, true) && Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success && CallCount() > BeforePerson, TEXT("same person properties honor requested reload"));
        Client->Group(TEXT("company"), TEXT("flags-company"));
        Client->ReloadFeatureFlagsAsync().Wait();
        FPostHogProperties Group; Group.SetNumber(TEXT("size"), 10);
        Check(Client->SetGroupPropertiesForFlags(TEXT("company"), Group, false) && Client->HasLoadedFeatureFlags(), TEXT("group targeting keeps flags readable without reload"));
        Check(PersistedWithoutCache(), TEXT("stale cache is not persisted under the new group context"));
        const int32 BeforeGroup = CallCount();
        Check(Client->SetGroupPropertiesForFlags(TEXT("company"), Group, true) && Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success && CallCount() > BeforeGroup, TEXT("same group properties honor requested reload"));
        Check(Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success, TEXT("targeting reload"));
        FFileHelper::LoadFileToString(Text, *StatePath);
        Saved = PostHogJson::Decode(Text); const TSharedPtr<FJsonObject>* Context = nullptr;
        Check(Saved && Saved->TryGetObjectField(TEXT("flag_context"), Context) && (*Context)->GetObjectField(TEXT("person_properties"))->GetStringField(TEXT("plan")) == TEXT("pro") && (*Context)->GetObjectField(TEXT("group_properties"))->GetObjectField(TEXT("company"))->GetNumberField(TEXT("size")) == 10, TEXT("target context persisted with cache"));
        auto Pending = Client->ReloadFeatureFlagsAsync();
        Check(Client->OptOutAsync().Get(), TEXT("opt out during flags"));
        const auto Disabled = Pending.Get();
        Check(Disabled.Status == EPostHogFlagReloadStatus::Disabled && Disabled.Flags.IsEmpty() && !Client->GetFeatureFlagResult(TEXT("bool-value")).bFound, TEXT("opt-out rejects pending completion and reads"));
        const int32 WhileDisabled = CallCount();
        Check(Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Disabled && CallCount() == WhileDisabled, TEXT("opted-out reload does not request"));
        Check(Client->OptInAsync().Get(), TEXT("opt-in flags"));
        Check(Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success, TEXT("opt-in refreshes"));
        Control(TEXT("/__control/state"), TEXT(R"({"delays":{"flags":0},"force500":{"flags":true}})"));
        Check(Client->ResetAsync().Get(), TEXT("reset flag identity"));
        const auto ResetLoad = Client->ReloadFeatureFlagsAsync().Get();
        Check(ResetLoad.Status == EPostHogFlagReloadStatus::Failed && ResetLoad.Flags.IsEmpty() && !Client->HasLoadedFeatureFlags(), TEXT("failed reset refresh cannot reuse old cache"));
        Check(Loaded.Flags.ToNative()->GetBoolField(TEXT("bool-value")), TEXT("prior completion snapshots remain independent"));
        Control(TEXT("/__control/state"), TEXT(R"({"force500":{"flags":false},"delays":{"flags":0.2}})"));
        auto ClosingLoad = Client->ReloadFeatureFlagsAsync();
        auto Closed = Client->ShutdownAsync();
        Check(Closed.Get() && ClosingLoad.Get().Status == EPostHogFlagReloadStatus::Closed, TEXT("shutdown joins and invalidates flags"));
        Client.Reset();
        Control(TEXT("/__control/state"), TEXT(R"({"delays":{"flags":0}})"));
        Config.ClientName = TEXT("pressure"); Config.BeforeSend = nullptr;
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("cache pressure client")); if (!Client) { return Result; }
        FPostHogProperties Large;
        Large.SetString(TEXT("large"), FString::ChrN(1024 * 1024 - 4096, TEXT('x'))); Client->Register(Large);
        Control(TEXT("/__control/state"), TEXT("{\"flags\":{\"string-value\":\"") + FString::ChrN(20000, TEXT('y')) + TEXT("\"}}"));
        const auto Pressure = Client->ReloadFeatureFlagsAsync().Get();
        Check(Pressure.Status == EPostHogFlagReloadStatus::Success && !Pressure.bPersisted && Client->HasLoadedFeatureFlags(), TEXT("oversized optional cache stays in memory"));
        Check(Client->OptOutAsync().Get() && Client->ShutdownAsync().Get(), TEXT("cache pressure cannot block consent persistence")); Client.Reset();
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client && Client->IsOptedOut() && !Client->HasLoadedFeatureFlags(), TEXT("consent survives cache pressure restart"));
        if (Client) { Client->ShutdownAsync().Get(); Client.Reset(); }
        Control(TEXT("/__control/state"), TEXT(R"({"flags":{"string-value":"test"}})"));
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return Result;
    });
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForFlagTests(this, MoveTemp(Future)));
    return true;
}
#endif
