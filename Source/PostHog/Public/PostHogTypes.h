#pragma once
#include "CoreMinimal.h"
#include "PostHogTypes.generated.h"

UENUM(BlueprintType)
enum class EPostHogPersonProfiles : uint8 { Never, IdentifiedOnly, Always };
UENUM(BlueprintType)
enum class EPostHogSessionMode : uint8 { Automatic, Manual };
UENUM(BlueprintType)
enum class EPostHogFlagReporting : uint8 { Default, Send, Silent };

/** Local SDK/adapter outcome. Ok confirms invocation, not mutation or delivery. */
UENUM(BlueprintType)
enum class EPostHogResult : uint8
{
    Ok, Accepted, InvalidInput, Closed, InternalError, UnsupportedAbi, Disabled, Full,
    OutputTooLarge, OptedOut, Dropped, Reentrant, Deferred, Inactive, WrongRecorder, Stale,
    InvalidResponse, TaskUnavailable
};
UENUM(BlueprintType)
enum class EPostHogPersistenceOutcome : uint8
{
    Persisted, MemoryOnly, Unavailable, WriteFailed, TimedOut, Deferred, Disabled, NotRequested, Unknown
};
UENUM(BlueprintType)
enum class EPostHogDeliveryOutcome : uint8
{
    Attempted, Stopped, Unavailable, Closed, Deferred, Disabled, Unknown
};

/** Local progress only; no outcome acknowledges ingestion.
 * State/Delivery describe an SDK report only when Status is Ok; bJoined tracks adapter teardown. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogDrainResult
{
    GENERATED_BODY()
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogResult Status = EPostHogResult::Closed;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogPersistenceOutcome State = EPostHogPersistenceOutcome::NotRequested;
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogDeliveryOutcome Delivery = EPostHogDeliveryOutcome::Closed;
    /** True only after shutdown joined native workers and admitted host calls. */
    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    bool bJoined = false;
};

/** Host viewport cadence/size and Rust encoding/capacity. StartSessionReplay starts recording.
 * Automatic viewport content is unredacted. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogReplaySettings
{
    GENERATED_BODY()
    /** Automatic owning-viewport capture cadence; Rust recording must be manually started. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "100"))
    int32 IntervalMs = 1000;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "64"))
    int32 MaxDimension = 960;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "1", ClampMax = "100"))
    int32 JpegQuality = 65;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "1"))
    int32 MaxFramesInFlight = 2;
};
