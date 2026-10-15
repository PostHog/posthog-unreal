#include "RunnerEnvironment.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"

void RunnerEnvironment::Spawn(UWorld* World)
{
	ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 1500), FRotator(-35, -25, 0));
	Sun->GetLightComponent()->SetIntensity(5.f);
	ASkyLight* Sky = World->SpawnActor<ASkyLight>();
	Sky->GetLightComponent()->SetIntensity(1.5f);
	Sky->GetLightComponent()->SetRealTimeCaptureEnabled(true);
	World->SpawnActor<ASkyAtmosphere>();
	AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>();
	Fog->GetComponent()->SetFogDensity(0.008f);
}
