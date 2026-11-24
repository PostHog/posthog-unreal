#pragma once
#include "CoreMinimal.h"
#include "PostHogTypes.generated.h"

UENUM(BlueprintType)
enum class EPostHogPersonProfiles : uint8
{
    Never,
    IdentifiedOnly,
    Always
};

UENUM(BlueprintType)
enum class EPostHogSessionMode : uint8
{
    Automatic,
    Manual
};

UENUM(BlueprintType)
enum class EPostHogFlagReporting : uint8
{
    Default,
    Send,
    Silent
};

UENUM(BlueprintType)
enum class EPostHogCaptureResult : uint8
{
    Accepted,
    Disabled,
    OptedOut,
    Closed,
    Invalid,
    Dropped,
    Reentrant
};

UENUM(BlueprintType)
enum class EPostHogPersistenceOutcome : uint8
{
    Persisted,
    MemoryOnly,
    Unavailable,
    WriteFailed,
    TimedOut,
    Deferred,
    Disabled,
    NotRequested,
    Unknown
};

UENUM(BlueprintType)
enum class EPostHogDeliveryOutcome : uint8
{
    Attempted,
    Stopped,
    TimedOut,
    Unavailable,
    Closed,
    Deferred,
    Disabled,
    Unknown
};

/** Local progress for a flush or shutdown operation (does not guarantee server ingestion). */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogDrainResult
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogPersistenceOutcome State = EPostHogPersistenceOutcome::NotRequested;

    UPROPERTY(BlueprintReadOnly, Category = "PostHog")
    EPostHogDeliveryOutcome Delivery = EPostHogDeliveryOutcome::Closed;
};

/**
 * Viewport replay settings. Recording must still be started via StartSessionReplay.
 * Note: viewport replay currently captures unredacted pixels (no UI/text masking is applied).
 */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogReplaySettings
{
    GENERATED_BODY()

    /**
     * Minimum time between replay frames in milliseconds (0 disables Rust throttling).
     * Viewport GPU readback enforces a 100 ms floor regardless of lower values.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "0"))
    int32 IntervalMs = 1000;

    /** Maximum width or height of captured replay frames in pixels. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "64"))
    int32 MaxDimension = 960;

    /** JPEG quality (1-100). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "1", ClampMax = "100"))
    int32 JpegQuality = 65;

    /** Maximum replay frames in flight inside the Rust encoder/delivery pipeline. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "1"))
    int32 MaxFramesInFlight = 2;
};
