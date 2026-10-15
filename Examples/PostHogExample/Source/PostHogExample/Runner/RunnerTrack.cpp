#include "RunnerTrack.h"
#include "Components/SplineComponent.h"

ARunnerTrack::ARunnerTrack()
{
	Spline = CreateDefaultSubobject<USplineComponent>(TEXT("CentreLine"));
	Spline->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	RootComponent = Spline;
}

void ARunnerTrack::PlanTo(float Distance)
{
	while (Pieces.IsEmpty() || Pieces.Last().StartDistance + Pieces.Last().Length <= Distance) AppendPiece();
}

void ARunnerTrack::AppendPiece()
{
	if (Pieces.IsEmpty())
	{
		Spline->ClearSplinePoints(false);
		AddSplinePoint(EndLocation, FVector::ZeroVector, FVector::ZeroVector);
	}
	const int32 StartPoint = Spline->GetNumberOfSplinePoints() - 1;
	FRunnerPiece& Piece = Pieces.AddDefaulted_GetRef();
	Piece.Location = EndLocation;
	Piece.Yaw = EndYaw;
	Piece.Turn = Course.Next();
	const FRotator Heading(0.f, EndYaw, 0.f);

	if (Piece.Turn == 0)
	{
		Piece.SegmentIndex = NextSegmentIndex++;
		const FVector Tangent = Heading.Vector() * RunnerRules::SegmentLength;
		SetLastLeaveTangent(Tangent);
		EndLocation += Tangent;
		AddSplinePoint(EndLocation, Tangent, Tangent);
	}
	else
	{
		// Two 45-degree Hermite spans with these tangents trace the quarter circle to well under a centimeter.
		const float Radius = RunnerRules::CornerRadius;
		const float Side = static_cast<float>(Piece.Turn);
		const float Handle = 4.f * FMath::Tan(UE_PI / 16.f) * Radius;
		SetLastLeaveTangent(Heading.Vector() * Handle);
		for (const float Angle : {UE_PI / 4.f, UE_HALF_PI})
		{
			const FVector Local(Radius * FMath::Sin(Angle), Side * Radius * (1.f - FMath::Cos(Angle)), 0.f);
			const FVector Tangent = Heading.RotateVector(FVector(FMath::Cos(Angle), Side * FMath::Sin(Angle), 0.f)) * Handle;
			AddSplinePoint(Piece.Location + Heading.RotateVector(Local), Tangent, Tangent);
		}
		EndLocation = Piece.Location + Heading.RotateVector(FVector(Radius, Side * Radius, 0.f));
		EndYaw = FRotator::NormalizeAxis(EndYaw + 90.f * Side);
	}

	Spline->UpdateSpline();
	Piece.StartDistance = Spline->GetDistanceAlongSplineAtSplinePoint(StartPoint);
	Piece.Length = Spline->GetSplineLength() - Piece.StartDistance;
}

void ARunnerTrack::AddSplinePoint(const FVector& Location, const FVector& ArriveTangent, const FVector& LeaveTangent)
{
	const int32 Index = Spline->GetNumberOfSplinePoints();
	Spline->AddPoint(FSplinePoint(Index, Location, ArriveTangent, LeaveTangent), false);
}

void ARunnerTrack::SetLastLeaveTangent(const FVector& Tangent)
{
	const int32 Last = Spline->GetNumberOfSplinePoints() - 1;
	Spline->SetTangentsAtSplinePoint(Last, Spline->GetArriveTangentAtSplinePoint(Last, ESplineCoordinateSpace::Local),
		Tangent, ESplineCoordinateSpace::Local, false);
}

FRunnerTrackFrame ARunnerTrack::FrameAt(const FVector& WorldLocation) const
{
	FRunnerTrackFrame Frame;
	if (Pieces.IsEmpty()) return Frame;
	const float Key = Spline->FindInputKeyClosestToWorldLocation(WorldLocation);
	Frame.Center = Spline->GetLocationAtSplineInputKey(Key, ESplineCoordinateSpace::World);
	const FVector Forward = Spline->GetDirectionAtSplineInputKey(Key, ESplineCoordinateSpace::World).GetSafeNormal2D();
	if (!Forward.IsZero()) Frame.Forward = Forward;
	Frame.Right = FVector(-Frame.Forward.Y, Frame.Forward.X, 0.f);
	Frame.Distance = Spline->GetDistanceAlongSplineAtSplineInputKey(Key);
	return Frame;
}

FVector ARunnerTrack::LocationAt(float Distance, int32 Lane, float Z)
{
	PlanTo(Distance + RunnerRules::SegmentLength);
	const FVector Center = Spline->GetLocationAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World);
	const FVector Forward = Spline->GetDirectionAtDistanceAlongSpline(Distance, ESplineCoordinateSpace::World).GetSafeNormal2D();
	return Center + FVector(-Forward.Y, Forward.X, 0.f) * RunnerRules::LaneY(Lane) + FVector(0.f, 0.f, Z);
}

int32 ARunnerTrack::LaneAt(const FVector& WorldLocation) const
{
	const FRunnerTrackFrame Frame = FrameAt(WorldLocation);
	const float Offset = FVector::DotProduct(WorldLocation - Frame.Center, Frame.Right);
	return RunnerRules::ClampLane(FMath::RoundToInt(Offset / RunnerRules::LaneWidth));
}
