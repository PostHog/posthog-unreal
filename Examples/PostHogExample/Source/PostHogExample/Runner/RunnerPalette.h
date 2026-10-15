#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "RunnerPalette.generated.h"

class UMaterialInstanceDynamic;
class UMaterialInterface;
class UStaticMesh;

/** Per-world cache of the engine basic shapes and tinted materials shared by every track segment. */
UCLASS()
class URunnerPaletteSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static URunnerPaletteSubsystem& Get(const UObject* WorldContext);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	UMaterialInstanceDynamic* Tint(bool bEmissive, FLinearColor Color);
	UMaterialInstanceDynamic* Paint(FLinearColor Color) { return Tint(false, Color); }
	// The emissive engine material lacks the instanced-mesh usage flag, so glowing pieces must be plain components.
	UMaterialInstanceDynamic* Glow(FLinearColor Color) { return Tint(true, Color); }

	UPROPERTY()
	TObjectPtr<UStaticMesh> Cube;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Sphere;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Cylinder;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Cone;

private:
	UPROPERTY()
	TObjectPtr<UMaterialInterface> PaintBase;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> GlowBase;
	UPROPERTY()
	TMap<FLinearColor, TObjectPtr<UMaterialInstanceDynamic>> Paints;
	UPROPERTY()
	TMap<FLinearColor, TObjectPtr<UMaterialInstanceDynamic>> Glows;
};
