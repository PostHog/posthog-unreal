#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "PostHogProperties.h"
#include "RunnerGameInstance.generated.h"

class UPostHogSubsystem;

/** The subsystem owns the client; this example instance keeps early gameplay events until setup completes. */
UCLASS()
class URunnerGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override;
    virtual void Shutdown() override;
    void Capture(const FString& Event, const FPostHogProperties& Properties);
    bool UseHardMode() const;
    FString AnalyticsStatus() const;

private:
    struct FPendingEvent { FString Name; FPostHogProperties Properties; };
    TArray<FPendingEvent> Pending;
    UPROPERTY()
    TObjectPtr<UPostHogSubsystem> PostHog;
    UFUNCTION()
    void SetupCompleted(bool bSuccess);
};
