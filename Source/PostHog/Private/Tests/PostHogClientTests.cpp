#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include "HAL/Event.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include <atomic>
#include <limits>
#if !PLATFORM_EXCEPTIONS_DISABLED
#include <stdexcept>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientBasicTest, "PostHog.Client.Basic",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogClientBasicTest::RunTest(const FString&)
{
    const FString Root = FPaths::ProjectSavedDir() / TEXT("PostHogTests") / FGuid::NewGuid().ToString();
    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_test_client");
    Config.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Config.Host.IsEmpty()) { Config.Host = TEXT("http://127.0.0.1:9"); }
    Config.StorageRoot = Root;
    Config.bPreloadFeatureFlags = false;
    Config.FlushAt = 100;
    Config.FlushIntervalMilliseconds = 60000;
    Config.ShutdownTimeoutMilliseconds = 500;
    Config.Context.SetString(TEXT("suite"), TEXT("client"));

    auto HookCalls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
    Config.BeforeSend = [HookCalls](FJsonObject& Event)
    {
        HookCalls->fetch_add(1);
        FString Name;
        if (Event.TryGetStringField(TEXT("event"), Name) && Name == TEXT("dropped_event")) { return false; }
        return true;
    };

    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid() && Client->IsInitialized() && !Client->IsDisabled());
    if (!Client) { return false; }

    const FString AnonId = Client->GetAnonymousId();
    TestFalse(TEXT("anonymous id generated"), AnonId.IsEmpty());
    TestEqual(TEXT("initial distinct id matches anonymous id"), Client->GetDistinctId(), AnonId);

    FPostHogProperties Props;
    Props.SetString(TEXT("weapon"), TEXT("bow"));
    Props.SetNumber(TEXT("damage"), 42.5);
    Props.SetBoolean(TEXT("crit"), true);
    TestEqual(TEXT("capture accepted"), Client->Capture(TEXT("attack"), Props), EPostHogCaptureResult::Accepted);
    TestEqual(TEXT("dropped by before_send"), Client->Capture(TEXT("dropped_event")), EPostHogCaptureResult::Dropped);
    TestEqual(TEXT("invalid empty event"), Client->Capture(TEXT("")), EPostHogCaptureResult::Invalid);
    TestTrue(TEXT("before_send ran"), HookCalls->load() >= 2);

    Client->Identify(TEXT("player-7"));
    TestEqual(TEXT("identified distinct id"), Client->GetDistinctId(), FString(TEXT("player-7")));

    Client->OptOut();
    TestTrue(TEXT("opted out"), Client->IsOptedOut());
    TestEqual(TEXT("capture while opted out"), Client->Capture(TEXT("ignored")), EPostHogCaptureResult::OptedOut);

    Client->OptIn();
    TestFalse(TEXT("opted back in"), Client->IsOptedOut());
    TestEqual(TEXT("screen accepted"), Client->Screen(TEXT("MainMenu")), EPostHogCaptureResult::Accepted);

    Client->Reset();
    TestNotEqual(TEXT("reset rotates anonymous id"), Client->GetAnonymousId(), AnonId);
    TestEqual(TEXT("reset restores anonymous distinct id"), Client->GetDistinctId(), Client->GetAnonymousId());

    const auto Drain = Client->ShutdownAsync().Get();
    TestFalse(TEXT("closed after shutdown"), Client->IsInitialized());
    TestEqual(TEXT("capture after shutdown"), Client->Capture(TEXT("after_close")), EPostHogCaptureResult::Closed);
    TestTrue(TEXT("persistence completed"), Drain.State == EPostHogPersistenceOutcome::Persisted || Drain.State == EPostHogPersistenceOutcome::TimedOut);

    Client.Reset();
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientDisabledTest, "PostHog.Client.Disabled",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogClientDisabledTest::RunTest(const FString&)
{
    FPostHogConfig Config;
    Config.ProjectToken = TEXT(""); // blank token disables client
    Config.bPreloadFeatureFlags = false;
    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid());
    if (!Client) { return false; }

    TestTrue(TEXT("blank token disables client"), Client->IsDisabled());
    TestEqual(TEXT("capture returns disabled"), Client->Capture(TEXT("noop")), EPostHogCaptureResult::Disabled);
    const auto Drain = Client->ShutdownAsync().Get();
    TestEqual(TEXT("disabled shutdown state"), Drain.State, EPostHogPersistenceOutcome::Disabled);
    TestEqual(TEXT("disabled shutdown delivery"), Drain.Delivery, EPostHogDeliveryOutcome::Disabled);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientBeforeSendAndCleanupTest, "PostHog.Client.BeforeSendAndCleanup",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogClientBeforeSendAndCleanupTest::RunTest(const FString&)
{
    // Verify non-finite numbers serialize safely as JSON null without invalidating properties.
    FPostHogProperties NonFinite;
    TestTrue(TEXT("NaN accepted as null"), NonFinite.SetNumber(TEXT("nan_val"), std::numeric_limits<double>::quiet_NaN()));
    TestTrue(TEXT("+Inf accepted as null"), NonFinite.SetNumber(TEXT("pos_inf"), std::numeric_limits<double>::infinity()));
    TestTrue(TEXT("-Inf accepted as null"), NonFinite.SetNumber(TEXT("neg_inf"), -std::numeric_limits<double>::infinity()));
    TestTrue(TEXT("non-finite properties valid"), NonFinite.IsValid());
    const auto NativeObj = NonFinite.ToNative();
    TestTrue(TEXT("nan serialized as null"), NativeObj->HasField(TEXT("nan_val")) && NativeObj->TryGetField(TEXT("nan_val"))->IsNull());
    TestTrue(TEXT("+inf serialized as null"), NativeObj->HasField(TEXT("pos_inf")) && NativeObj->TryGetField(TEXT("pos_inf"))->IsNull());
    TestTrue(TEXT("-inf serialized as null"), NativeObj->HasField(TEXT("neg_inf")) && NativeObj->TryGetField(TEXT("neg_inf"))->IsNull());

    // Verify oversized (>4 MB) values fail cleanly without crashing or corrupting existing keys.
    const FString Huge = FString::ChrN(4 * 1024 * 1024 + 64, TEXT('a'));
    FPostHogProperties Bounded;
    TestTrue(TEXT("initial key set before oversized write"), Bounded.SetString(TEXT("keep"), TEXT("ok")));
    TestFalse(TEXT("oversized string rejected by SetString"), Bounded.SetString(TEXT("keep"), Huge));
    TestEqual(TEXT("existing key preserved after oversized SetString"), Bounded.ToJson(), FString(TEXT("{\"keep\":\"ok\"}")));
    TestFalse(TEXT("oversized array element produces invalid value"), FPostHogValue::Array({FPostHogValue::String(Huge)}).IsValid());

    struct FDestroyTracker
    {
        std::atomic<int32> Destroyed{0};
        FEvent* AllDestroyed = nullptr;
        int32 Expected = 2;
        ~FDestroyTracker()
        {
            if (AllDestroyed) { FPlatformProcess::ReturnSynchEventToPool(AllDestroyed); }
        }
    };
    struct FCaptureSentinel
    {
        TSharedRef<FDestroyTracker, ESPMode::ThreadSafe> Tracker;
        ~FCaptureSentinel()
        {
            if (Tracker->Destroyed.fetch_add(1) + 1 >= Tracker->Expected && Tracker->AllDestroyed)
            {
                Tracker->AllDestroyed->Trigger();
            }
        }
    };

    auto Tracker = MakeShared<FDestroyTracker, ESPMode::ThreadSafe>();
    Tracker->AllDestroyed = FPlatformProcess::GetSynchEventFromPool(true);

    auto BeforeSendSentinel = MakeShared<FCaptureSentinel, ESPMode::ThreadSafe>(FCaptureSentinel{Tracker});
    auto FlagsSentinel = MakeShared<FCaptureSentinel, ESPMode::ThreadSafe>(FCaptureSentinel{Tracker});

    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_test_hook");
    Config.Host = TEXT("http://127.0.0.1:9");
    Config.bPreloadFeatureFlags = false;
    Config.FlushAt = 100;
    Config.FlushIntervalMilliseconds = 60000;
    Config.ShutdownTimeoutMilliseconds = 500;

    auto MutatedSeen = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Config.BeforeSend = [BeforeSendSentinel = MoveTemp(BeforeSendSentinel), MutatedSeen](FJsonObject& Event)
    {
        (void)BeforeSendSentinel;
        FString Name;
        Event.TryGetStringField(TEXT("event"), Name);
#if !PLATFORM_EXCEPTIONS_DISABLED
        if (Name == TEXT("throw_event"))
        {
            throw std::runtime_error("secret_should_not_be_logged");
        }
#endif
        if (Name == TEXT("mutate_ts"))
        {
            if (const TSharedPtr<FJsonObject>* PropsObj = nullptr; Event.TryGetObjectField(TEXT("properties"), PropsObj) && PropsObj)
            {
                (*PropsObj)->SetBoolField(TEXT("mutated"), true);
            }
            Event.SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
            MutatedSeen->store(true);
        }
        return true;
    };
    Config.OnFeatureFlagsChanged = [FlagsSentinel = MoveTemp(FlagsSentinel)](FPostHogFeatureFlagsUpdate)
    {
        (void)FlagsSentinel;
    };

    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid() && Client->IsInitialized());
    if (!Client) { return false; }

    TestEqual(TEXT("mutated event with ISO8601 timestamp accepted"),
        Client->Capture(TEXT("mutate_ts"), NonFinite), EPostHogCaptureResult::Accepted);
    TestTrue(TEXT("before_send mutated event"), MutatedSeen->load());

#if !PLATFORM_EXCEPTIONS_DISABLED
    AddExpectedMessage(TEXT("BeforeSend threw std::exception"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
    TestEqual(TEXT("throwing before_send drops event"),
        Client->Capture(TEXT("throw_event")), EPostHogCaptureResult::Dropped);
#endif

    Client->ShutdownAsync().Get();
    Client.Reset();
    TestTrue(TEXT("callback captures destroyed cleanly after shutdown and reset"), Tracker->AllDestroyed->Wait(5000));
    TestEqual(TEXT("both callback captures destroyed"), Tracker->Destroyed.load(), 2);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogClientOptOutStopsReplayTest, "PostHog.Client.OptOutStopsReplay",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogClientOptOutStopsReplayTest::RunTest(const FString&)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Host.IsEmpty())
    {
        AddInfo(TEXT("Skipping OptOut replay test: POSTHOG_TEST_HOST is not set."));
        return true;
    }

    const FString Root = FPaths::ProjectSavedDir() / TEXT("PostHogTests") / FGuid::NewGuid().ToString();
    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_test_optout_replay");
    Config.Host = Host;
    Config.StorageRoot = Root;
    Config.bPreloadFeatureFlags = false;
    Config.ShutdownTimeoutMilliseconds = 500;

    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid() && Client->IsInitialized());
    if (!Client)
    {
        IFileManager::Get().DeleteDirectory(*Root, false, true);
        return false;
    }

    Client->StartSessionReplay();
    const double Deadline = FPlatformTime::Seconds() + 5.0;
    while (!Client->IsSessionReplayActive() && FPlatformTime::Seconds() < Deadline)
    {
        FPlatformProcess::Sleep(0.02f);
    }
    TestTrue(TEXT("replay active before opt-out"), Client->IsSessionReplayActive());

    Client->OptOut();
    TestFalse(TEXT("replay stopped by opt-out"), Client->IsSessionReplayActive());
    Client->StartSessionReplay();
    TestFalse(TEXT("start replay ignored while opted out"), Client->IsSessionReplayActive());

    Client->OptIn();
    TestFalse(TEXT("replay remains stopped after opt-in"), Client->IsSessionReplayActive());

    Client->ShutdownAsync().Get();
    Client.Reset();
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif
