#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "PostHogSmokeGameInstance.generated.h"

class SWidget;
class SWindow;

/** Asset-free automatic-rendered consumer, enabled only by the isolated replay driver. */
UCLASS()
class UPostHogSmokeGameInstance : public UGameInstance
{
    GENERATED_BODY()
public:
    virtual void Init() override;
    virtual void Shutdown() override;
private:
    bool TickSmoke(float DeltaTime);
    FTSTicker::FDelegateHandle TickHandle;
    TSharedPtr<SWidget> Pattern;
    TSharedPtr<SWindow> OtherWindow;
    FString Controls;
    bool bManualStart = false;
    bool bReplayStarted = false;
    double Deadline = 0;
    bool bResized = false;
    bool bHalted = false;
    bool bResumed = false;
    bool bCaptured = false;
};
