#pragma once
#include "CoreMinimal.h"
#include "PostHogTypes.generated.h"

UENUM(BlueprintType)
enum class EPostHogPersonProfiles : uint8 { Never, IdentifiedOnly, Always };
UENUM(BlueprintType)
enum class EPostHogSessionMode : uint8 { Automatic, Explicit };
UENUM(BlueprintType)
enum class EPostHogCaptureResult : uint8 { Accepted, Disabled, Invalid, QueueFull, Closed };

/** Screenshot replay proof of concept. Desktop only, no masking: frames show everything on screen.
 * Frames are recorded only when the server also enables replay. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogReplaySettings
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay")
    bool bEnabled = false;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "100"))
    int32 IntervalMs = 1000;
    /** Longest edge of the uploaded JPEG; the back buffer is scaled down to fit. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "64"))
    int32 MaxDimension = 960;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Replay", meta = (ClampMin = "1", ClampMax = "100"))
    int32 JpegQuality = 65;
};
