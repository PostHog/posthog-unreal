#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "posthog_unreal.h"

namespace
{
FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Output;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<>::Create(&Output));
    return Output;
}

struct FNativeResult
{
    TArray<FString> Errors;
};

class FWaitForNative final : public IAutomationLatentCommand
{
public:
    FWaitForNative(FAutomationTestBase* InTest, TFuture<FNativeResult>&& InFuture)
        : Test(InTest), Future(MoveTemp(InFuture)) {}

    virtual bool Update() override
    {
        if (!Future.IsReady()) { return false; }
        for (const FString& Error : Future.Get().Errors) { Test->AddError(Error); }
        return true;
    }

private:
    FAutomationTestBase* Test;
    TFuture<FNativeResult> Future;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogNativeTest, "PostHog.NativeIntegration",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FPostHogNativeTest::RunTest(const FString& Parameters)
{
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    if (Host.IsEmpty())
    {
        AddError(TEXT("Set POSTHOG_TEST_HOST to the SDK mock server URL."));
        return false;
    }
    const FString Storage = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("PostHogTests") / FGuid::NewGuid().ToString());
    TFuture<FNativeResult> Future = Async(EAsyncExecution::Thread, [Host, Storage]()
    {
        FNativeResult Result;
        auto Check = [&Result](phu_status Actual, phu_status Expected, const TCHAR* Operation)
        {
            if (Actual != Expected) { Result.Errors.Add(FString::Printf(TEXT("%s returned %d, expected %d"), Operation, Actual, Expected)); }
        };
        TSharedRef<FJsonObject> Options = MakeShared<FJsonObject>();
        Options->SetStringField(TEXT("project_token"), TEXT("phc_MOCK"));
        Options->SetStringField(TEXT("host"), Host);
        Options->SetStringField(TEXT("storage_path"), Storage);
        Options->SetNumberField(TEXT("shutdown_timeout_ms"), 1000);
        phu_handle Handle = 0;
        const FTCHARToUTF8 OptionsUtf8(*Encode(Options));
        Check(phu_create(PHU_ABI_VERSION, {reinterpret_cast<const uint8_t*>(OptionsUtf8.Get()), static_cast<size_t>(OptionsUtf8.Length())}, &Handle), PHU_OK, TEXT("create"));
        if (Handle == 0) { return Result; }

        TSharedRef<FJsonObject> Event = MakeShared<FJsonObject>();
        Event->SetStringField(TEXT("event"), TEXT("unreal_native_integration"));
        Event->SetStringField(TEXT("distinct_id"), TEXT("unreal-test-player"));
        Event->SetStringField(TEXT("uuid"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphens));
        Event->SetStringField(TEXT("timestamp"), FDateTime::UtcNow().ToIso8601());
        TSharedRef<FJsonObject> Properties = MakeShared<FJsonObject>();
        Properties->SetStringField(TEXT("$lib"), TEXT("posthog-unreal"));
        Properties->SetStringField(TEXT("$lib_version"), TEXT("0.0.1"));
        Properties->SetBoolField(TEXT("$process_person_profile"), false);
        Event->SetObjectField(TEXT("properties"), Properties);
        const FTCHARToUTF8 EventUtf8(*Encode(Event));
        Check(phu_capture(Handle, {reinterpret_cast<const uint8_t*>(EventUtf8.Get()), static_cast<size_t>(EventUtf8.Length())}), PHU_ACCEPTED, TEXT("capture"));
        Check(phu_flush(Handle), PHU_OK, TEXT("flush"));

        TSharedRef<FJsonObject> Context = MakeShared<FJsonObject>();
        Context->SetStringField(TEXT("distinct_id"), TEXT("unreal-test-player"));
        const FTCHARToUTF8 ContextUtf8(*Encode(Context));
        phu_buffer Flags = {nullptr, 0};
        Check(phu_flags(Handle, {reinterpret_cast<const uint8_t*>(ContextUtf8.Get()), static_cast<size_t>(ContextUtf8.Length())}, &Flags), PHU_OK, TEXT("flags"));
        if (Flags.data)
        {
            const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Flags.data), static_cast<int32>(Flags.len));
            TSharedPtr<FJsonObject> Parsed;
            if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(FString(Text.Length(), Text.Get())), Parsed) || !Parsed.IsValid() || !Parsed->HasField(TEXT("flags")))
            {
                Result.Errors.Add(TEXT("Native flag export was not valid JSON with a flags map."));
            }
        }
        phu_buffer_free(Flags);
        Check(phu_close(Handle), PHU_OK, TEXT("close"));
        Check(phu_close(Handle), PHU_CLOSED, TEXT("repeated close"));
        IFileManager::Get().DeleteDirectory(*Storage, false, true);
        return Result;
    });
    FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FWaitForNative>(this, MoveTemp(Future)));
    return true;
}

#endif
