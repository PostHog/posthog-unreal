#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogTestHelpers.h"
#include "PostHogNative.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFlagsTest, "PostHog.Flags", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogFlagsTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors; auto Check = [&Errors](bool Pass, const TCHAR* Message) { if (!Pass) { Errors.Add(Message); } };
        auto Config = PostHogTests::Config(TEXT("adapter-flags")); Config.bPreloadFeatureFlags = true;
        Config.bSendFeatureFlagEvents = false;
        int32 Exposures = 0;
        Config.BeforeSend = [&Exposures](FJsonObject& Event) { if (Event.GetStringField(TEXT("event")) == TEXT("$feature_flag_called")) { ++Exposures; } return true; };

        auto Client = FPostHogClient::CreateAsync(Config).Get(); if (!Client) { Errors.Add(TEXT("flag client creation")); return Errors; }
        Client->FlushAsync().Get(); FPostHogFeatureFlagsUpdate Update;
        Check(Client->PollFeatureFlags(Update) == EPostHogResult::Ok, TEXT("polled automatic notification"));
        Check(Update.Snapshot.bAvailable && Update.Snapshot.Flags.Contains(TEXT("bool-value")) && !Update.Snapshot.Flags.Contains(TEXT("disabled-flag")), TEXT("notification retains SDK enabled-only shape"));
        const auto All = Client->GetFeatureFlags();
        Check(All.Status == EPostHogResult::Ok && All.Flags.Contains(TEXT("disabled-flag")) && !All.Flags[TEXT("disabled-flag")].bEnabled, TEXT("getter includes known false"));
        const auto Missing = Client->GetFeatureFlagResult(TEXT("missing"), EPostHogFlagReporting::Silent);
        Check(Missing.Status == EPostHogResult::Ok && !Missing.bFound, TEXT("missing is not a failed read"));
        const auto Variant = Client->GetFeatureFlagResult(TEXT("string-value"), EPostHogFlagReporting::Silent);
        Check(Variant.bFound && Variant.bHasPayload && Variant.Payload.ToJson() == TEXT("\"test\""), TEXT("decoded payload string"));
        Client->GetFeatureFlagResult(TEXT("disabled-flag"));
        Check(Exposures == 0, TEXT("default reporting respects core configuration"));
        Client->GetFeatureFlagResult(TEXT("disabled-flag"), EPostHogFlagReporting::Send);
        Check(Exposures == 1, TEXT("explicit reporting overrides core default"));
        Check(Client->ReloadFeatureFlags() == EPostHogResult::Ok, TEXT("fire-and-forget reload"));
        Client->FlushAsync().Get(); Check(Client->PollFeatureFlags(Update) == EPostHogResult::Ok, TEXT("manual update notification"));
        Check(Client->ShutdownAsync().Get().bJoined, TEXT("flag client close"));
        Check(Client->PollFeatureFlags(Update) == EPostHogResult::Closed, TEXT("close cancels unread notification"));
        return Errors;
    });
}
#endif
