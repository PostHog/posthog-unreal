#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "RunnerHUD.generated.h"

/** Asset-free HUD for the runner prototype. */
UCLASS()
class ARunnerHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void DrawHUD() override;
};
