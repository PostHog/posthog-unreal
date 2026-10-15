#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RunnerTrackSegment.generated.h"

class UStaticMesh;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/** One disposable section of track, including its obstacles, coins, and scenery. */
UCLASS()
class ARunnerTrackSegment : public AActor
{
	GENERATED_BODY()

public:
	ARunnerTrackSegment();
	void Build(int32 SegmentIndex);

private:
	UPROPERTY()
	TObjectPtr<UStaticMesh> Cube;
	UPROPERTY()
	TObjectPtr<UStaticMesh> Sphere;
	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> Materials;

	UMaterialInstanceDynamic* MakeMaterial(FLinearColor Color);
	UStaticMeshComponent* AddBlock(FVector Position, FVector Size, UMaterialInstanceDynamic* Material, bool bCollision);
	void AddCoin(FVector Position, UMaterialInstanceDynamic* Material);
};
