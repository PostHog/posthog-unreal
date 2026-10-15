#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RunnerTrackSegment.generated.h"

class FRunnerRuins;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/** One disposable section of track, including its obstacles, relics, and ruins. */
UCLASS()
class ARunnerTrackSegment : public AActor
{
	GENERATED_BODY()

public:
	ARunnerTrackSegment();
	void Build(int32 SegmentIndex);

private:
	UStaticMeshComponent* AddBlock(FVector Position, FVector Size, UMaterialInstanceDynamic* Material, bool bVisible = true);
	void AddCoin(FVector Position, FRunnerRuins& Ruins);
	void AddHazards(int32 SafeLane, FRandomStream& Random, FRunnerRuins& Ruins);
};
