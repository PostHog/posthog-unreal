#include "RunnerEnvironment.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/SkyLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/World.h"

namespace
{
	void SpawnSun(UWorld* World)
	{
		// Low sun ahead-left: glows through the haze and throws long shadows down the path.
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 1500), FRotator(-22.f, 150.f, 0.f));
		ULightComponent* Light = Sun->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(12.f);
		Light->SetLightColor(FLinearColor(1.f, 0.78f, 0.52f));
	}

	void SpawnSky(UWorld* World)
	{
		// Chasm sits below the planet surface; height fog handles the near scene so aerial perspective does not darken it.
		ASkyAtmosphere* Atmosphere = World->SpawnActor<ASkyAtmosphere>();
		Atmosphere->GetComponent()->SetAerialPerspectiveStartDepth(2.f);

		ASkyLight* Sky = World->SpawnActor<ASkyLight>();
		USkyLightComponent* Light = Sky->GetLightComponent();
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensity(1.4f);
		Light->SetLightColor(FLinearColor(0.72f, 0.9f, 1.f));
		Light->SetRealTimeCaptureEnabled(true);
	}

	void SpawnFog(UWorld* World)
	{
		// Second dense layer pools at the chasm floor so the drop reads as mist; the causeway stays clear.
		AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, -800), FRotator::ZeroRotator);
		UExponentialHeightFogComponent* Component = Fog->GetComponent();
		Component->SetFogDensity(0.05f);
		Component->SetFogHeightFalloff(2.f);
		Component->SetStartDistance(600.f);
		Component->SetSecondFogDensity(1.f);
		Component->SetSecondFogHeightOffset(-1700.f);
		Component->SetSecondFogHeightFalloff(3.f);
		Component->SetFogInscatteringColor(FLinearColor(1.25f, 1.02f, 0.76f));
		Component->SetDirectionalInscatteringColor(FLinearColor(2.5f, 1.5f, 0.6f));
		Component->SetDirectionalInscatteringExponent(4.f);
	}

	void SpawnGrade(UWorld* World)
	{
		APostProcessVolume* Volume = World->SpawnActor<APostProcessVolume>();
		Volume->bUnbound = true;
		FPostProcessSettings& Settings = Volume->Settings;
		Settings.bOverride_WhiteTemp = true;
		Settings.WhiteTemp = 7200.f;
		Settings.bOverride_AutoExposureBias = true;
		Settings.AutoExposureBias = -0.2f;
		Settings.bOverride_ColorSaturation = true;
		Settings.ColorSaturation = FVector4(1.06f, 1.06f, 1.06f, 1.f);
		Settings.bOverride_VignetteIntensity = true;
		Settings.VignetteIntensity = 0.3f;
	}
}

void RunnerEnvironment::Spawn(UWorld* World)
{
	SpawnSun(World);
	SpawnSky(World);
	SpawnFog(World);
	SpawnGrade(World);
}
