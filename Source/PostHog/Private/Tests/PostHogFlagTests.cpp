#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include "HAL/Event.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"
#include <atomic>
#if !PLATFORM_EXCEPTIONS_DISABLED
#include <stdexcept>
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFlagLookupTest, "PostHog.Flags.LookupAndNotification",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogFlagLookupTest::RunTest(const FString&)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_test_flags");
    Config.Host = Host.IsEmpty() ? TEXT("http://127.0.0.1:9") : Host;
    Config.bPreloadFeatureFlags = !Host.IsEmpty();
    Config.ShutdownTimeoutMilliseconds = 500;

    struct FSharedFlagWaitState
    {
        FCriticalSection Mutex;
        FEvent* UpdatedEvent = FPlatformProcess::GetSynchEventFromPool(true);
        FPostHogFeatureFlagsUpdate Received;

        ~FSharedFlagWaitState()
        {
            if (UpdatedEvent)
            {
                FPlatformProcess::ReturnSynchEventToPool(UpdatedEvent);
            }
        }
    };

    auto WaitState = MakeShared<FSharedFlagWaitState, ESPMode::ThreadSafe>();
    Config.OnFeatureFlagsChanged = [WaitState](FPostHogFeatureFlagsUpdate Update)
    {
        if (Update.Snapshot.bAvailable)
        {
            {
                FScopeLock Lock(&WaitState->Mutex);
                WaitState->Received = MoveTemp(Update);
            }
            if (WaitState->UpdatedEvent)
            {
                WaitState->UpdatedEvent->Trigger();
            }
        }
    };

    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid());
    if (!Client)
    {
        return false;
    }

    const auto Missing = Client->GetFeatureFlagResult(TEXT("nonexistent-flag"), EPostHogFlagReporting::Silent);
    TestFalse(TEXT("missing flag not found"), Missing.bFound);
    TestTrue(TEXT("missing flag returns caller default"), Client->IsFeatureEnabled(TEXT("nonexistent-flag"), EPostHogFlagReporting::Silent, true));

    if (!Host.IsEmpty())
    {
        TestTrue(TEXT("received remote flags update"), WaitState->UpdatedEvent->Wait(5000));
        const auto All = Client->GetFeatureFlags();
        TestTrue(TEXT("flags map available"), All.bAvailable);
    }

    Client->ShutdownAsync().Get();
    Client.Reset();
    return true;
}

#if !PLATFORM_EXCEPTIONS_DISABLED
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFlagCallbackExceptionTest, "PostHog.Flags.CallbackException",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogFlagCallbackExceptionTest::RunTest(const FString&)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Host.IsEmpty())
    {
        AddInfo(TEXT("Skipping flag callback exception test: POSTHOG_TEST_HOST is not set."));
        return true;
    }

    struct FSharedState
    {
        std::atomic<bool> bThrew{false};
        FEvent* Called = FPlatformProcess::GetSynchEventFromPool(true);
        FEvent* Destroyed = FPlatformProcess::GetSynchEventFromPool(true);

        ~FSharedState()
        {
            if (Called) { FPlatformProcess::ReturnSynchEventToPool(Called); }
            if (Destroyed) { FPlatformProcess::ReturnSynchEventToPool(Destroyed); }
        }
    };
    struct FDestroySentinel
    {
        TSharedRef<FSharedState, ESPMode::ThreadSafe> State;
        ~FDestroySentinel()
        {
            if (State->Destroyed) { State->Destroyed->Trigger(); }
        }
    };

    auto State = MakeShared<FSharedState, ESPMode::ThreadSafe>();
    auto Sentinel = MakeShared<FDestroySentinel, ESPMode::ThreadSafe>(FDestroySentinel{State});

    AddExpectedMessage(TEXT("OnFeatureFlagsChanged threw std::exception."), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);

    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_test_flags_throw");
    Config.Host = Host;
    Config.bPreloadFeatureFlags = true;
    Config.ShutdownTimeoutMilliseconds = 500;
    Config.OnFeatureFlagsChanged = [State, Sentinel = MoveTemp(Sentinel)](FPostHogFeatureFlagsUpdate)
    {
        (void)Sentinel;
        if (!State->bThrew.exchange(true))
        {
            if (State->Called) { State->Called->Trigger(); }
            throw std::runtime_error("secret_flag_exception");
        }
    };

    auto Client = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
    TestTrue(TEXT("client created"), Client.IsValid());
    if (!Client) { return false; }

    TestTrue(TEXT("throwing flag listener invoked"), State->Called->Wait(5000));

    Client->ShutdownAsync().Get();
    Client.Reset();
    TestTrue(TEXT("flag listener completed and cleaned up"), State->Destroyed->Wait(5000));
    return true;
}
#endif
#endif
