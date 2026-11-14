#pragma once
#include "PostHogClient.h"
#include "posthog_unreal.h"

/** Native value/ownership conversion only; no SDK policy. */
namespace PostHogNative
{
    struct FBuffer
    {
        phu_buffer Value{nullptr, 0};
        ~FBuffer() { phu_buffer_free(Value); }
        FBuffer() = default;
        FBuffer(const FBuffer&) = delete;
        FBuffer& operator=(const FBuffer&) = delete;
        TSharedPtr<FJsonValue> Read() const;
    };
    phu_bytes View(const FTCHARToUTF8& Text);
    EPostHogResult Result(phu_status Status);
    TSharedRef<FJsonObject> Operation(const TCHAR* Name);
    FPostHogDrainResult Drain(phu_status Status, const FBuffer& Buffer);
    FPostHogFeatureFlagResult Flag(const TSharedPtr<FJsonValue>& Value);
    FPostHogFeatureFlags Flags(const TSharedPtr<FJsonValue>& Value);
    FPostHogFeatureFlagsUpdate Update(const TSharedPtr<FJsonValue>& Value);
}
