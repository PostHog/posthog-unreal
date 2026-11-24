#pragma once

#include "CoreMinimal.h"
#include "PostHogSubsystem.h"
#include "PostHogBlueprintProbe.generated.h"

/** Example-side receiver for reflected delegate automation. */
UCLASS(Transient)
class UPostHogBlueprintProbe : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION()
    void OnFlagsChanged(const FPostHogFeatureFlagsUpdate& Update)
    {
        ++Completions;
        bGameThread = bGameThread && IsInGameThread();
        LastUpdate = Update;
    }

    int32 Completions = 0;
    bool bGameThread = true;
    FPostHogFeatureFlagsUpdate LastUpdate;
};
