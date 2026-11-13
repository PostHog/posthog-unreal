#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogFlagCache.h"
#include "PostHogJson.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogFailedFlagCacheTest, "PostHog.FailedFlagCache",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogFailedFlagCacheTest::RunTest(const FString& Parameters)
{
    // These are normalized export records, not the upstream /flags wire format.
    for (bool bPartial : {false, true})
    {
        FPostHogFlagCache Cache;
        auto Initial = PostHogJson::Decode(TEXT(R"({"flags":{
            "failed-bool":{"value":true,"payload":{"copy":"local"}},
            "failed-disabled":{"value":false,"payload":{"copy":"local"}},
            "failed-variant":{"value":"blue","payload":{"copy":"local"}}
            },"request_id":"initial","errors_while_computing":false,"quota_limited":false})"));
        TestEqual(TEXT("initial load"), Cache.Apply(Initial.ToSharedRef()), EPostHogFlagReloadStatus::Success);
        auto Response = PostHogJson::Decode(TEXT(R"({"flags":{
            "failed-bool":{"value":false,"failed":true,"payload":{"copy":"placeholder"}},
            "failed-disabled":{"value":false,"failed":true,"payload":{"copy":"placeholder"}},
            "failed-variant":{"value":"red","failed":true,"payload":{"copy":"placeholder"}},
            "failed-fresh":{"value":false,"failed":true,"payload":true},
            "disabled":{"value":false},"explicit-healthy":{"value":false,"failed":false},
            "healthy":{"value":"green","payload":{"copy":"healthy"}}
            },"request_id":"refresh","errors_while_computing":false,"quota_limited":false})"));
        Response->SetBoolField(TEXT("errors_while_computing"), bPartial);
        TestEqual(TEXT("refresh status"), Cache.Apply(Response.ToSharedRef()),
            bPartial ? EPostHogFlagReloadStatus::Partial : EPostHogFlagReloadStatus::Success);
        TestFalse(TEXT("fresh failure has no result"), Cache.Flags.Contains(TEXT("failed-fresh")));
        TestFalse(TEXT("fresh failure omitted from bulk/enrichment"), Cache.Values().ToNative()->HasField(TEXT("failed-fresh")));
        TestTrue(TEXT("healthy false without failure marker"), Cache.Flags.Contains(TEXT("disabled")) &&
            !Cache.Flags[TEXT("disabled")].Value.ToNative()->AsBool());
        TestTrue(TEXT("explicit healthy false"), Cache.Flags.Contains(TEXT("explicit-healthy")) &&
            !Cache.Flags[TEXT("explicit-healthy")].Value.ToNative()->AsBool());
        TestEqual(TEXT("healthy sibling payload"), Cache.Flags[TEXT("healthy")].Payload.ToJson(), FString(TEXT("{\"copy\":\"healthy\"}")));
        if (bPartial)
        {
            TestTrue(TEXT("partial refresh keeps successful boolean"), Cache.Flags[TEXT("failed-bool")].Value.ToNative()->AsBool());
            TestTrue(TEXT("partial refresh keeps successful false"), Cache.Flags.Contains(TEXT("failed-disabled")) &&
                !Cache.Flags[TEXT("failed-disabled")].Value.ToNative()->AsBool());
            TestEqual(TEXT("partial refresh keeps successful variant"), Cache.Flags[TEXT("failed-variant")].Value.ToJson(), FString(TEXT("\"blue\"")));
            for (const FString& Key : {FString(TEXT("failed-bool")), FString(TEXT("failed-disabled")), FString(TEXT("failed-variant"))})
            {
                const auto& Flag = Cache.Flags[Key];
                TestEqual(TEXT("partial refresh keeps paired payload"), Flag.Payload.ToJson(), FString(TEXT("{\"copy\":\"local\"}")));
                TestEqual(TEXT("retained value keeps source metadata"), Flag.Metadata.ToNative()->GetStringField(TEXT("request_id")), FString(TEXT("initial")));
                TestFalse(TEXT("unused failure does not taint retained exposure"), FPostHogFlagCache::Exposure(Key, Flag).ToNative()->HasField(TEXT("$feature_flag_error")));
            }
        }
        else
        {
            TestFalse(TEXT("complete refresh removes failed stale boolean"), Cache.Flags.Contains(TEXT("failed-bool")));
            TestFalse(TEXT("complete refresh removes failed stale false"), Cache.Flags.Contains(TEXT("failed-disabled")));
            TestFalse(TEXT("complete refresh removes failed stale variant"), Cache.Flags.Contains(TEXT("failed-variant")));
        }
        FPostHogFlagCache Restored;
        TestTrue(TEXT("filtered cache restores"), Restored.Restore(Cache.Serialize()));
        TestFalse(TEXT("failure payload is not persisted"), Restored.Serialize().ToNative()->HasField(TEXT("failed-fresh")));
    }

    FPostHogFlagCache Cache;
    auto AllFailed = PostHogJson::Decode(TEXT(R"({"flags":{
        "placeholder":{"failed":true},"variant":{"value":"red","failed":true,"payload":"placeholder"}
        },"errors_while_computing":false,"quota_limited":false})"));
    TestEqual(TEXT("all failed is a complete empty success"), Cache.Apply(AllFailed.ToSharedRef()), EPostHogFlagReloadStatus::Success);
    TestTrue(TEXT("all failed has no values or payloads"), Cache.bLoaded && Cache.Flags.IsEmpty() && Cache.Serialize().IsEmpty());
    FPostHogProperties Saved;
    TestTrue(TEXT("saved fixture parses"), FPostHogProperties::TryParse(TEXT(R"({"failed":{"value":false,"failed":true,"payload":"placeholder"},"healthy":{"value":false}})"), Saved));
    TestTrue(TEXT("restore filters failed records"), Cache.Restore(Saved));
    TestTrue(TEXT("restore keeps healthy false only"), Cache.Flags.Num() == 1 && Cache.Flags.Contains(TEXT("healthy")));
    return true;
}
#endif
