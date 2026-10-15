#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RunnerTrackSegment.generated.h"

class FRunnerRuins;
struct FRunnerPiece;
class UStaticMeshComponent;
class UMaterialInstanceDynamic;

/** One disposable section of track: a straight with obstacles, relics, and ruins, or a hazard-free corner tile. */
UCLASS()
class ARunnerTrackSegment : public AActor
{
	GENERATED_BODY()

public:
	ARunnerTrackSegment();
	void Build(int32 SegmentIndex);
	/** Square tile whose centre line bends 90 degrees to the right (+1) or left (-1). */
	void BuildCorner(int32 InTurn);
	/** Removes decoration that would stand in the path of nearby pieces heading another way. Returns the count removed. */
	int32 TrimScenery(TConstArrayView<FRunnerPiece> Pieces);

	/** Distance along the track's centre line where this piece begins. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	float StartDistance = 0.f;
	/** Centre-line length of this piece. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	float TrackLength = 0.f;
	/** 0 for a straight, +1 / -1 for a right / left corner. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	int32 Turn = 0;

private:
	UStaticMeshComponent* AddBlock(FVector Position, FVector Size, UMaterialInstanceDynamic* Material, bool bVisible = true);
	void AddCoin(FVector Position, FRunnerRuins& Ruins, float Yaw = 0.f);
	void AddHazards(int32 SafeLane, FRandomStream& Random, FRunnerRuins& Ruins);
};
