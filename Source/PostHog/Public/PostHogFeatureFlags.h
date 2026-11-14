#pragma once
#include "CoreMinimal.h"
#include "PostHogProperties.h"
#include "PostHogTypes.h"
#include "PostHogFeatureFlags.generated.h"

/** Owned SDK evaluation snapshot. Missing differs from known false; null payloads stay present. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlagResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogResult Status = EPostHogResult::Closed;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bFound = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bEnabled = false;
    /** Boolean or variant string, including an empty variant. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Value;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bHasPayload = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Payload;
    /** Complete SDK record, including producing context and optional evaluation metadata. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Record;
};

USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlags
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogResult Status = EPostHogResult::Closed;
    /** False is unavailable; true with an empty map is a known empty snapshot. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bAvailable = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    TMap<FString, FPostHogFeatureFlagResult> Flags;
};

/** Last received, coalesced SDK notification, not request-correlated reload completion.
 * Context describes its evaluation, not necessarily current targeting when delivered. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlagsUpdate
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogFeatureFlags Snapshot;
    /** Notifications include enabled flags only; GetFeatureFlags includes known false. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Context;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bErrorsLoading = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bPartial = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bInvalidPayloads = false;
    /** JSON null or the SDK's sanitized {kind,status?} load error. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Error;
};
