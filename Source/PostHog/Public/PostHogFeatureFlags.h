#pragma once

#include "CoreMinimal.h"
#include "PostHogProperties.h"
#include "PostHogFeatureFlags.generated.h"

UENUM(BlueprintType)
enum class EPostHogFlagReloadStatus : uint8
{
    Failed, Success, Partial, QuotaLimited, Disabled, Closed, Stale
};

/** Missing flags differ from known false flags; JSON null payloads remain present. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlagResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bFound = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Value;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bHasPayload = false;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Payload;
    /** Evaluation metadata from the response that supplied this flag. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Metadata;
};

USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFlagReloadResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogFlagReloadStatus Status = EPostHogFlagReloadStatus::Failed;
    /** Cache is usable in memory even when its optional disk snapshot cannot fit. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bPersisted = false;
    /** Completion-time values, including known false. Failures retain only
     * values for the same context; disabled/closed/stale results are empty. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Flags;

    /** Native context stamp used to reject stale deferred notifications. */
    uint64 ContextGeneration = 0;
};
