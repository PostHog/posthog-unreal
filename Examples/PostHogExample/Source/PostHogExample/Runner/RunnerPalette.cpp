#include "RunnerPalette.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"

URunnerPaletteSubsystem& URunnerPaletteSubsystem::Get(const UObject* WorldContext)
{
	URunnerPaletteSubsystem* Palette = WorldContext->GetWorld()->GetSubsystem<URunnerPaletteSubsystem>();
	check(Palette);
	return *Palette;
}

void URunnerPaletteSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube"));
	Sphere = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
	Cylinder = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder"));
	Cone = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cone"));
	PaintBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/EngineMaterials/EmissiveMeshMaterial"));
}

UMaterialInstanceDynamic* URunnerPaletteSubsystem::Tint(bool bEmissive, FLinearColor Color)
{
	TMap<FLinearColor, TObjectPtr<UMaterialInstanceDynamic>>& Cache = bEmissive ? Glows : Paints;
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Cache.Find(Color)) return *Found;
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(bEmissive ? GlowBase : PaintBase, this);
	Material->SetVectorParameterValue(TEXT("Color"), Color);
	Cache.Add(Color, Material);
	return Material;
}
