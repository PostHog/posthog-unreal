#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "RunnerRules.h"
#include "RunnerTrack.generated.h"

class USplineComponent;

/** One planned piece of course: a straight segment or a corner tile. */
struct FRunnerPiece
{
	FVector Location = FVector::ZeroVector;
	float Yaw = 0.f;
	float StartDistance = 0.f;
	float Length = 0.f;
	/** 0 for a straight, +1 / -1 for a right / left corner. */
	int32 Turn = 0;
	/** Content index of a straight segment; corners do not consume one. */
	int32 SegmentIndex = INDEX_NONE;
};

/** The track's centre line and lateral axis at the point closest to a location. */
struct FRunnerTrackFrame
{
	FVector Center = FVector::ZeroVector;
	FVector Forward = FVector::ForwardVector;
	FVector Right = FVector::RightVector;
	float Distance = 0.f;
};

/** Plans the course and keeps its centre line as a spline. Segment actors are spawned by the game mode. */
UCLASS()
class ARunnerTrack : public AActor
{
	GENERATED_BODY()

public:
	ARunnerTrack();

	/** Appends pieces until the planned course is longer than Distance. */
	void PlanTo(float Distance);
	const TArray<FRunnerPiece>& GetPieces() const { return Pieces; }

	FRunnerTrackFrame FrameAt(const FVector& WorldLocation) const;
	FVector LocationAt(float Distance, int32 Lane, float Z);
	int32 LaneAt(const FVector& WorldLocation) const;

private:
	UPROPERTY()
	TObjectPtr<USplineComponent> Spline;

	TArray<FRunnerPiece> Pieces;
	RunnerRules::FCourse Course;
	FVector EndLocation = FVector::ZeroVector;
	float EndYaw = 0.f;
	int32 NextSegmentIndex = 0;

	void AppendPiece();
	void AddSplinePoint(const FVector& Location, const FVector& ArriveTangent, const FVector& LeaveTangent);
	void SetLastLeaveTangent(const FVector& Tangent);
};
