#pragma once

#include "CoreMinimal.h"

class AActor;
class UInstancedStaticMeshComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class URunnerPaletteSubsystem;
class USceneComponent;
class UStaticMesh;
class UStaticMeshComponent;

namespace RunnerPalette
{
	inline const FLinearColor Dirt(0.36f, 0.24f, 0.11f);
	inline const FLinearColor Masonry(0.25f, 0.23f, 0.19f);
}

namespace RunnerRuins
{
	constexpr float PostOffset = 125.f;
	constexpr float HazardHalfDepth = 51.f;

	UStaticMeshComponent* AttachMesh(AActor* Owner, USceneComponent* Parent, UStaticMesh* Mesh,
		UMaterialInterface* Material, FVector Position, FVector Size, FRotator Rotation, FName CollisionProfile,
		bool bVisible = true);
}

/**
 * Dresses one track segment as temple ruins on a causeway cut through dense jungle. Decorative pieces are
 * collision-free and batched per (shape, color).
 */
class FRunnerRuins
{
public:
	FRunnerRuins(AActor* InOwner, URunnerPaletteSubsystem& InPalette, int32 Seed);

	void Path();
	void Edges(bool bArch);
	void Chasm();
	/** A distant jungle mass, or occasionally a cliff with a waterfall, rising out of the mist. */
	void MistTower(float X, float Side, float Height);
	void Statue(float X, float Side, bool bSeated);
	void Torch(float X, float Side);

	void FallenPillar(FVector Center);
	void CrackedPosts(FVector BeamCenter);
	void CarvedLintel(FVector Center);
	void BlockStack(FVector Center);
	void Relic(USceneComponent* Trigger);

private:
	AActor* Owner;
	URunnerPaletteSubsystem& Palette;
	FRandomStream Random;
	TMap<TPair<UStaticMesh*, UMaterialInstanceDynamic*>, UInstancedStaticMeshComponent*> Batches;

	template <int32 N>
	FLinearColor Pick(const FLinearColor (&Choices)[N]) { return Choices[Random.RandRange(0, N - 1)]; }
	float Jitter(float Range) { return Random.FRandRange(-Range, Range); }

	void Add(UStaticMesh* Mesh, FLinearColor Color, FVector Center, FVector Size,
		FRotator Rotation = FRotator::ZeroRotator);
	void Box(FLinearColor Color, FVector Center, FVector Size, FRotator Rotation = FRotator::ZeroRotator);
	void Attach(USceneComponent* Parent, UStaticMesh* Mesh, UMaterialInstanceDynamic* Material, FVector Position,
		FVector Size, FRotator Rotation = FRotator::ZeroRotator);

	void Slab(float X0, float X1, float Y0, float Y1, bool bEdge);
	float Column(FVector Base, float PlinthWidth, float Diameter, float Height);
	void Rubble(FVector Center, float MinChunk, float MaxChunk);
	void Pillar(float X, float Side, float Height, bool bBroken);
	void CarvedPanel(float X, float Side);
	void Arch();
	void Vines(FVector Top, FVector Spread, int32 Count, float MinLength, float MaxLength);
	void Jungle(float Side);
	void Undergrowth(float Side);
	void Tree(FVector Base, float Height, float Diameter);
	void Bough(FVector Root, FVector Tip);
	void Canopy(FVector Center, float Size, float MinBottom);
	void Overhang();
	void Log(FVector Center, float Length, float Yaw);
	void BroadLeaves(FVector Center, float Side);
	void Foliage(FVector Center, float Size);
	void Ferns(FVector Center);
	void Inset(FVector FaceCenter, FVector Normal, float Width, float Height);
};
