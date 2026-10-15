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
    void OnReload(const FPostHogFlagReloadResult& Result)
    {
        ++Completions;
        bGameThread = bGameThread && IsInGameThread();
        LastResult = Result;
        if (bReloadAgain && Subsystem.IsValid())
        {
            bReloadAgain = false;
            Subsystem->ReloadFeatureFlags();
        }
    }

    TWeakObjectPtr<UPostHogSubsystem> Subsystem;
    int32 Completions = 0;
    bool bGameThread = true;
    bool bReloadAgain = false;
    FPostHogFlagReloadResult LastResult;
};
