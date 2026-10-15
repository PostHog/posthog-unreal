#pragma once
#include "CoreMinimal.h"
#include "PostHogTypes.generated.h"

UENUM(BlueprintType)
enum class EPostHogPersonProfiles : uint8 { Never, IdentifiedOnly, Always };
UENUM(BlueprintType)
enum class EPostHogSessionMode : uint8 { Automatic, Explicit };
UENUM(BlueprintType)
enum class EPostHogCaptureResult : uint8 { Accepted, Disabled, Invalid, QueueFull, Closed };
