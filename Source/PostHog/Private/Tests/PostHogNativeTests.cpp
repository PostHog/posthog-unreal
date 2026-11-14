#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogTestHelpers.h"
#include "PostHogNative.h"
#include "PostHogJson.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogNativeTest, "PostHog.NativeIntegration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogNativeTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors;
        auto Check = [&Errors](bool Pass, const TCHAR* Message) { if (!Pass) { Errors.Add(Message); } };
        auto Options = MakeShared<FJsonObject>(); Options->SetStringField(TEXT("project_token"), TEXT("phc_MOCK"));
        Options->SetStringField(TEXT("host"), FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST")));
        Options->SetBoolField(TEXT("preload_feature_flags"), false); Options->SetNumberField(TEXT("shutdown_timeout_ms"), 1000);
        const FTCHARToUTF8 OptionsText(*PostHogJson::Encode(Options)); phu_handle Handle = 0;
        Check(phu_create(PHU_ABI_VERSION, PostHogNative::View(OptionsText), {nullptr, nullptr}, &Handle) == PHU_OK, TEXT("header ABI v3 create"));
        if (!Handle) { return Errors; }
        const FTCHARToUTF8 Event(TEXT("{\"event\":\"unreal_native_integration\"}"));
        PostHogNative::FBuffer Receipt;
        Check(phu_capture(Handle, PostHogNative::View(Event), &Receipt.Value) == PHU_ACCEPTED && Receipt.Read().IsValid(), TEXT("stateful input and owned receipt"));
        PostHogNative::FBuffer Drain;
        Check(phu_flush(Handle, &Drain.Value) == PHU_OK && PostHogNative::Drain(PHU_OK, Drain).Delivery == EPostHogDeliveryOutcome::Attempted, TEXT("drain report shape"));
        PostHogNative::FBuffer Closed;
        Check(phu_close(Handle, &Closed.Value) == PHU_OK, TEXT("native close"));
        PostHogNative::FBuffer Repeated;
        Check(phu_close(Handle, &Repeated.Value) == PHU_CLOSED && !Repeated.Value.data, TEXT("invalidated handle and zeroed output"));
        return Errors;
    });
}
#endif
