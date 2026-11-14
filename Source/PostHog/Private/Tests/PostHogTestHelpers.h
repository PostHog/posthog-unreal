#pragma once
#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include "PostHogTasks.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"

namespace PostHogTests
{
inline FPostHogConfig Config(const TCHAR* Name)
{
    FPostHogConfig Out; Out.ProjectToken = TEXT("phc_MOCK"); Out.Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    Out.ClientName = Name; Out.bPreloadFeatureFlags = false; Out.FlushAt = 100; Out.FlushIntervalMilliseconds = 60000;
    Out.ShutdownTimeoutMilliseconds = 1000; return Out;
}
class FWait final : public IAutomationLatentCommand
{
public:
    FWait(FAutomationTestBase* InTest, TSharedFuture<TArray<FString>> InFuture) : Test(InTest), Future(MoveTemp(InFuture)) {}
    bool Update() override
    {
        if (!Future.IsReady()) { return false; }
        for (const FString& Error : Future.Get()) { Test->AddError(Error); }
        return true;
    }
private:
    FAutomationTestBase* Test;
    TSharedFuture<TArray<FString>> Future;
};
template <typename Work> bool Run(FAutomationTestBase* Test, Work&& Function)
{
    if (FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST")).IsEmpty()) { Test->AddError(TEXT("Run through Tests/native_integration.py with the mock server.")); return false; }
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FWait>(Test, PostHogTasks::Run(Forward<Work>(Function))));
    return true;
}
}
#endif
