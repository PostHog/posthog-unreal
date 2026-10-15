#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogClient.h"
#include "PostHogFlagCache.h"
#include "PostHogJson.h"
#include "Async/Async.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogReplayConfigTest, "PostHog.ReplayConfig",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogReplayConfigTest::RunTest(const FString& Parameters)
{
    FPostHogFlagCache Cache;
    auto Enabled = PostHogJson::Decode(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":false,"session_recording":{"endpoint":"/s/"}})"));
    TestEqual(TEXT("enabled response"), Cache.Apply(Enabled.ToSharedRef()), EPostHogFlagReloadStatus::Success);
    TestTrue(TEXT("object enables replay"), Cache.SessionRecording.IsSet() && Cache.SessionRecording->ToNative()->GetStringField(TEXT("endpoint")) == TEXT("/s/"));
    FPostHogFlagCache Restored;
    Restored.RestoreSessionRecording(Cache.SessionRecording->ToNative());
    TestTrue(TEXT("restore keeps config"), Restored.SessionRecording.IsSet());
    auto IsSetAfterEnabled = [&Cache, &Enabled](const TCHAR* Json)
    {
        Cache.Apply(Enabled.ToSharedRef());
        Cache.Apply(PostHogJson::Decode(Json).ToSharedRef());
        return Cache.SessionRecording.IsSet();
    };
    TestFalse(TEXT("false disables replay"), IsSetAfterEnabled(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":false,"session_recording":false})")));
    TestFalse(TEXT("null disables replay"), IsSetAfterEnabled(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":false,"session_recording":null})")));
    TestFalse(TEXT("missing disables replay"), IsSetAfterEnabled(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":false})")));
    TestTrue(TEXT("quota-limited flags keep last replay config"), IsSetAfterEnabled(TEXT(R"({"flags":{},"errors_while_computing":false,"quota_limited":true})")));
    Cache.Clear();
    TestFalse(TEXT("clear disables replay"), Cache.SessionRecording.IsSet());
    return true;
}

namespace
{
class FWaitForReplayTests final : public IAutomationLatentCommand
{
public:
    FWaitForReplayTests(FAutomationTestBase* InTest, TFuture<TArray<FString>>&& InFuture) : Test(InTest), Future(MoveTemp(InFuture)) {}
    virtual bool Update() override
    {
        if (!Future.IsReady()) { return false; }
        for (const FString& Error : Future.Get()) { Test->AddError(Error); }
        return true;
    }
private:
    FAutomationTestBase* Test;
    TFuture<TArray<FString>> Future;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogReplaySubmitTest, "PostHog.ReplaySubmit",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogReplaySubmitTest::RunTest(const FString& Parameters)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Host.IsEmpty()) { AddError(TEXT("Set POSTHOG_TEST_HOST to the SDK mock server.")); return false; }
    const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogReplayTests") / FGuid::NewGuid().ToString());
    auto Future = Async(EAsyncExecution::Thread, [Host, Root]
    {
        TArray<FString> Errors;
        auto Check = [&Errors](bool bPass, const TCHAR* Message) { if (!bPass) { Errors.Add(Message); } };
        const TArray64<uint8> Jpeg = {0xFF, 0xD8, 0xFF, 0xD9};
        const FDateTime CapturedAt(2026, 10, 15, 12);
        FPostHogConfig Config; Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = Host; Config.StorageRoot = Root;
        Config.ClientName = TEXT("replay-off"); Config.bPreloadFeatureFlags = false;

        auto Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("create replay-disabled client")); if (!Client) { return Errors; }
        const FString Session = Client->GetSessionId();
        Check(!Client->IsSessionReplayAllowed(), TEXT("replay disallowed by default"));
        Check(Client->SubmitReplaySnapshot(Session, 1920, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Disabled, TEXT("submit disabled without local opt-in"));
        Check(Client->SubmitReplaySnapshot(FString(), 1920, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Invalid, TEXT("empty session is invalid"));
        Check(Client->SubmitReplaySnapshot(Session, 0, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Invalid, TEXT("empty size is invalid"));
        Check(Client->SubmitReplaySnapshot(Session, 1920, 1080, {}, CapturedAt) == EPostHogCaptureResult::Invalid, TEXT("empty image is invalid"));
        Check(Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success, TEXT("load flags without replay"));
        Check(!Client->IsSessionReplayAllowed(), TEXT("remote config alone does not enable replay"));
        Check(Client->ShutdownAsync().Get(), TEXT("close replay-disabled client")); Client.Reset();

        Config.ClientName = TEXT("replay-on"); Config.Replay.bEnabled = true;
        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("create replay client")); if (!Client) { return Errors; }
        Check(!Client->IsSessionReplayAllowed(), TEXT("replay waits for remote config"));
        Check(Client->SubmitReplaySnapshot(Client->GetSessionId(), 1920, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Accepted, TEXT("submit snapshot"));
        Check(Client->ReloadFeatureFlagsAsync().Get().Status == EPostHogFlagReloadStatus::Success, TEXT("load replay config"));
        Check(Client->IsSessionReplayAllowed(), TEXT("remote session_recording enables replay"));
        Check(Client->ShutdownAsync().Get(), TEXT("close replay client")); Client.Reset();

        Client = FPostHogClient::CreateAsync(Config).Get();
        Check(Client.IsValid(), TEXT("restart replay client")); if (!Client) { return Errors; }
        Check(Client->IsSessionReplayAllowed(), TEXT("persisted remote config survives restart"));
        Check(Client->OptOutAsync().Get(), TEXT("opt out"));
        Check(!Client->IsSessionReplayAllowed(), TEXT("opt-out disallows replay"));
        Check(Client->SubmitReplaySnapshot(Client->GetSessionId(), 1920, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Disabled, TEXT("opt-out blocks submit"));
        Check(Client->ShutdownAsync().Get(), TEXT("close opted-out replay client"));
        Check(Client->SubmitReplaySnapshot(TEXT("0199e5a0-0000-7000-8000-000000000000"), 1920, 1080, Jpeg, CapturedAt) == EPostHogCaptureResult::Closed, TEXT("closed client rejects submit"));
        return Errors;
    });
    ADD_LATENT_AUTOMATION_COMMAND(FWaitForReplayTests(this, MoveTemp(Future)));
    return true;
}
#endif
