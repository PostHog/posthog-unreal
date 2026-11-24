#pragma once
#include "CoreMinimal.h"
#include "PostHogProperties.h"
#include "PostHogTypes.h"
#include "PostHogFeatureFlags.generated.h"

/** Single feature-flag lookup result. Missing differs from known false; null payloads stay present. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlagResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bFound = false;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bEnabled = false;

    /** Boolean or variant string. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Value;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bHasPayload = false;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Payload;

    /** Complete SDK evaluation record (including context and metadata). */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Record;
};

USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlags
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bAvailable = false;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    TMap<FString, FPostHogFeatureFlagResult> Flags;
};

/** Snapshot delivered when feature flags are restored, bootstrapped, or loaded remotely. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogFeatureFlagsUpdate
{
    GENERATED_BODY()

    /** Enabled cached flags only; call GetFeatureFlag or GetFeatureFlags to inspect known-disabled flags. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogFeatureFlags Snapshot;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogProperties Context;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bErrorsLoading = false;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bPartial = false;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bInvalidPayloads = false;

    /** JSON null or `{kind, status?}` load error. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    FPostHogValue Error;
};
