#pragma once
#include "CoreMinimal.h"
#include "PostHogSubsystem.h"
#include "PostHogBlueprintProbe.generated.h"

UCLASS(Transient)
class UPostHogBlueprintProbe : public UObject
{
    GENERATED_BODY()
public:
    UFUNCTION()
    void OnUpdate(const FPostHogFeatureFlagsUpdate& Update)
    { ++Notifications; bGameThread &= IsInGameThread(); Last = Update; }
    UFUNCTION()
    void OnSetup(bool bSuccess) { ++SetupNotifications; bSetupSucceeded = bSuccess; bGameThread &= IsInGameThread(); }
    int32 SetupNotifications = 0;
    bool bSetupSucceeded = false;
    int32 Notifications = 0;
    bool bGameThread = true;
    FPostHogFeatureFlagsUpdate Last;
};
