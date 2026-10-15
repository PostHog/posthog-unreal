#include "RunnerTrackSegment.h"
#include "RunnerPalette.h"
#include "RunnerRules.h"
#include "RunnerRuins.h"
#include "RunnerTrack.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	const FLinearColor CornerSandstone[] = {
		FLinearColor(0.40f, 0.31f, 0.18f), FLinearColor(0.34f, 0.27f, 0.16f),
		FLinearColor(0.46f, 0.37f, 0.22f), FLinearColor(0.26f, 0.27f, 0.26f)};
	const FLinearColor CornerRut(0.20f, 0.13f, 0.06f);
	const FLinearColor CornerDeepStone(0.17f, 0.155f, 0.125f);
	const FLinearColor CornerMoss(0.07f, 0.14f, 0.03f);
	constexpr float ColonnadeOffset = 612.f;

	/** Collision-free corner dressing, batched per (shape, color) like the straight segments' ruins. */
	class FCornerDressing
	{
	public:
		FCornerDressing(AActor* InOwner, URunnerPaletteSubsystem& InPalette, int32 Seed)
			: Random(Seed), Owner(InOwner), Palette(InPalette) {}

		FRandomStream Random;

		FLinearColor Stone() { return CornerSandstone[Random.RandRange(0, UE_ARRAY_COUNT(CornerSandstone) - 1)]; }

		void Add(UStaticMesh* Mesh, FLinearColor Color, FVector Center, FVector Size, float Yaw = 0.f)
		{
			UMaterialInstanceDynamic* Material = Palette.Paint(Color);
			UInstancedStaticMeshComponent*& Batch = Batches.FindOrAdd({Mesh, Material});
			if (!Batch)
			{
				Batch = NewObject<UInstancedStaticMeshComponent>(Owner);
				Batch->SetupAttachment(Owner->GetRootComponent());
				Batch->SetStaticMesh(Mesh);
				Batch->SetMaterial(0, Material);
				Batch->SetCollisionProfileName(UCollisionProfile::NoCollision_ProfileName);
				Batch->RegisterComponent();
				Owner->AddInstanceComponent(Batch);
			}
			Batch->AddInstance(FTransform(FRotator(0.f, Yaw, 0.f), Center, Size / 100.f));
		}

		void Column(FVector Base, float Width, float Height)
		{
			Add(Palette.Cube, Stone(), Base + FVector(0.f, 0.f, 18.f), FVector(Width, Width, 36.f));
			Add(Palette.Cylinder, Stone(), Base + FVector(0.f, 0.f, 36.f + Height / 2.f), FVector(Width * 0.78f, Width * 0.78f, Height));
			Add(Palette.Cube, Stone(), Base + FVector(0.f, 0.f, 51.f + Height), FVector(Width + 6.f, Width + 6.f, 30.f));
			if (Random.FRand() < 0.5f) Add(Palette.Cube, CornerMoss, Base + FVector(0.f, 0.f, 68.f + Height), FVector(Width - 14.f, Width - 24.f, 4.f));
		}

		/** A ruined wall from A to B: uneven blocks with mossy tops. */
		void Wall(FVector A, FVector B)
		{
			const FVector Along = (B - A).GetSafeNormal();
			const float Yaw = Along.Rotation().Yaw;
			const float Length = FVector::Dist(A, B);
			for (float T = 0.f; T < Length;)
			{
				const float Piece = FMath::Min(Length - T, Random.FRandRange(90.f, 220.f));
				const float Height = Random.FRandRange(60.f, 230.f);
				const FVector Center = A + Along * (T + Piece / 2.f);
				Add(Palette.Cube, Stone(), Center + FVector(0.f, 0.f, Height / 2.f), FVector(Piece - 4.f, 50.f, Height), Yaw);
				if (Random.FRand() < 0.35f)
					Add(Palette.Cube, CornerMoss, Center + FVector(0.f, 0.f, Height + 2.f), FVector(Piece * 0.7f, 46.f, 4.f), Yaw);
				T += Piece;
			}
		}

	private:
		AActor* Owner;
		URunnerPaletteSubsystem& Palette;
		TMap<TPair<UStaticMesh*, UMaterialInstanceDynamic*>, UInstancedStaticMeshComponent*> Batches;
	};
}

ARunnerTrackSegment::ARunnerTrackSegment()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

UStaticMeshComponent* ARunnerTrackSegment::AddBlock(FVector Position, FVector Size,
	UMaterialInstanceDynamic* Material, bool bVisible)
{
	return RunnerRuins::AttachMesh(this, RootComponent, URunnerPaletteSubsystem::Get(this).Cube, Material, Position, Size,
		FRotator::ZeroRotator, UCollisionProfile::BlockAll_ProfileName, bVisible);
}

void ARunnerTrackSegment::AddCoin(FVector Position, FRunnerRuins& Ruins, float Yaw)
{
	USphereComponent* Trigger = NewObject<USphereComponent>(this);
	Trigger->SetupAttachment(RootComponent);
	Trigger->SetRelativeLocation(Position);
	Trigger->SetRelativeRotation(FRotator(0.f, Yaw, 0.f));
	Trigger->SetSphereRadius(65.f);
	Trigger->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Trigger->SetCollisionResponseToAllChannels(ECR_Ignore);
	Trigger->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	Trigger->SetGenerateOverlapEvents(true);
	Trigger->ComponentTags.Add(TEXT("RunnerCoin"));
	Trigger->RegisterComponent();
	AddInstanceComponent(Trigger);
	Ruins.Relic(Trigger);
}

void ARunnerTrackSegment::Build(int32 SegmentIndex)
{
	URunnerPaletteSubsystem& Palette = URunnerPaletteSubsystem::Get(this);
	FRandomStream Random(17389 + SegmentIndex);
	// Own stream: dressing edits must not shift obstacle draws.
	FRunnerRuins Ruins(this, Palette, 52711 + SegmentIndex);

	const float Length = RunnerRules::SegmentLength;
	AddBlock(FVector(Length / 2.f, 0.f, -25.f), FVector(Length, 1100.f, 50.f), Palette.Paint(RunnerPalette::Dirt));
	Ruins.Path();
	Ruins.Chasm();
	// Skyline draws must precede the lane draws to keep the course layout stable.
	for (float Side : {-1.f, 1.f})
	{
		for (float X = 400.f; X < Length; X += 800.f)
			Ruins.MistTower(X, Side, Random.FRandRange(650.f, 2000.f));
	}
	Ruins.Edges(SegmentIndex % 3 == 0);
	if (SegmentIndex % 3 == 1) Ruins.Statue(1200.f, SegmentIndex % 2 ? 1.f : -1.f, SegmentIndex % 4 == 1);
	Ruins.Torch(600.f, -1.f);
	Ruins.Torch(1800.f, 1.f);

	// Every row has a fully open lane. The opening is also marked by a line of relics.
	const int32 SafeLane = Random.RandRange(-1, 1);
	if (SegmentIndex > 0) AddHazards(SafeLane, Random, Ruins);
	const int32 CoinLane = SegmentIndex == 0 ? 0 : SafeLane;
	for (float X = 950.f; X <= 2150.f; X += 300.f)
		AddCoin(FVector(X, RunnerRules::LaneY(CoinLane), 95.f), Ruins);
}

void ARunnerTrackSegment::AddHazards(int32 SafeLane, FRandomStream& Random, FRunnerRuins& Ruins)
{
	UMaterialInstanceDynamic* Masonry = URunnerPaletteSubsystem::Get(this).Paint(RunnerPalette::Masonry);
	auto AddHazard = [&](FVector Position, FVector Size, bool bVisible)
	{
		AddBlock(Position, Size, Masonry, bVisible)->ComponentTags.Add(TEXT("RunnerHazard"));
	};
	for (int32 Lane = -1; Lane <= 1; ++Lane)
	{
		if (Lane == SafeLane) continue;
		const int32 Kind = Random.RandRange(0, 2);
		FVector Position(1400.f, RunnerRules::LaneY(Lane), 0.f);
		FVector Size(100.f, 220.f, 240.f);
		if (Kind == 0) // Jumpable low hurdle; the collision box stays authoritative and the fallen pillar only dresses it.
		{
			Size.Z = 80.f;
			Position.Z = 40.f;
			AddHazard(Position, Size, false);
			Ruins.FallenPillar(Position);
		}
		else if (Kind == 1) // Slide beneath a beam with 130 cm of clearance.
		{
			Size.Z = 110.f;
			Position.Z = 185.f;
			for (float Offset : {-RunnerRuins::PostOffset, RunnerRuins::PostOffset})
				AddHazard(Position + FVector(0.f, Offset, -65.f), FVector(100.f, 18.f, 240.f), true);
			AddHazard(Position, Size, true);
			Ruins.CrackedPosts(Position);
			Ruins.CarvedLintel(Position);
		}
		else
		{
			Position.Z = 120.f;
			AddHazard(Position, Size, true);
			Ruins.BlockStack(Position);
		}
	}
}

void ARunnerTrackSegment::BuildCorner(int32 InTurn)
{
	Turn = InTurn;
	URunnerPaletteSubsystem& Palette = URunnerPaletteSubsystem::Get(this);
	const int32 Seed = 61813 + FMath::RoundToInt(StartDistance);
	FCornerDressing Dressing(this, Palette, Seed);
	FRunnerRuins Ruins(this, Palette, Seed);

	// Local frame: the centre line enters at the origin heading +X and leaves through the side at (R, Side * R).
	const float Side = static_cast<float>(Turn);
	const float Radius = RunnerRules::CornerRadius;
	const float Size = RunnerRules::CornerSize;
	const auto ArcPoint = [Side, Radius](float ArcRadius, float Angle, float Z)
	{
		return FVector(ArcRadius * FMath::Sin(Angle), Side * (Radius - ArcRadius * FMath::Cos(Angle)), Z);
	};
	const auto ArcYaw = [Side](float Angle) { return Side * FMath::RadiansToDegrees(Angle); };
	// Lays out a band of pieces centred on a quarter circle around the corner's pivot.
	const auto Arc = [&](float ArcRadius, float MaxPiece, TFunctionRef<void(FVector, float, float, int32)> Place)
	{
		const int32 Count = FMath::Max(2, FMath::CeilToInt(ArcRadius * UE_HALF_PI / MaxPiece));
		const float Step = UE_HALF_PI / Count;
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const float Angle = (Index + 0.5f) * Step;
			Place(ArcPoint(ArcRadius, Angle, 0.f), ArcRadius * Step, ArcYaw(Angle), Index);
		}
	};

	AddBlock(FVector(Radius, 0.f, -25.f), FVector(Size, Size, 50.f), Palette.Paint(RunnerPalette::Dirt));
	Dressing.Add(Palette.Cube, CornerDeepStone, FVector(Radius + 30.f, 0.f, -355.f), FVector(Size + 60.f, Size + 120.f, 700.f));

	// Flagstones follow each lane around the bend; two bands per lane keep the wedges between stones small.
	for (int32 Lane = -1; Lane <= 1; ++Lane)
	{
		const float LaneRadius = Radius - Side * RunnerRules::LaneY(Lane);
		for (int32 Band = 0; Band < 2; ++Band)
		{
			Arc(LaneRadius + (Band ? 70.5f : -70.5f), 150.f, [&](FVector Center, float Length, float Yaw, int32 Index)
			{
				if (Dressing.Random.FRand() < 0.12f) return;
				// Alternate heights so stones that overlap on the inside of the curve never share a top face.
				const float Top = (Index + Band) % 2 ? 2.4f : 1.f;
				Dressing.Add(Palette.Cube, Dressing.Stone(), Center + FVector(0.f, 0.f, Top - 5.f), FVector(Length - 3.f, 135.f, 10.f), Yaw);
			});
		}
	}
	for (float Offset : {-450.f, -150.f, 150.f, 450.f})
	{
		const float Width = FMath::Abs(Offset) > 300.f ? 10.f : 18.f;
		Arc(Radius + Offset, 60.f, [&](FVector Center, float Length, float Yaw, int32)
		{
			Dressing.Add(Palette.Cube, CornerRut, Center, FVector(Length + 2.f, Width, 1.f), Yaw);
		});
	}
	for (float Offset : {-500.f, 500.f})
	{
		Arc(Radius + Offset, 140.f, [&](FVector Center, float Length, float Yaw, int32)
		{
			if (Dressing.Random.FRand() < 0.12f) return;
			Dressing.Add(Palette.Cube, Dressing.Stone(), Center + FVector(0.f, 0.f, Dressing.Random.FRandRange(0.3f, 3.f) - 5.f),
				FVector(Length - 3.f, 97.f, 10.f), Yaw);
		});
	}

	// Outer walls continue the straights' colonnade lines; a lone column marks the inside of the bend.
	const FVector Entry(0.f, -Side * ColonnadeOffset, 0.f);
	const FVector Elbow(Radius + ColonnadeOffset, -Side * ColonnadeOffset, 0.f);
	const FVector Exit(Radius + ColonnadeOffset, Side * Radius, 0.f);
	Dressing.Wall(Entry, Elbow);
	Dressing.Wall(Elbow, Exit);
	Dressing.Column(Elbow, 120.f, 520.f);
	Dressing.Column((Entry + Elbow) / 2.f, 104.f, Dressing.Random.FRandRange(380.f, 480.f));
	Dressing.Column((Elbow + Exit) / 2.f, 104.f, Dressing.Random.FRandRange(380.f, 480.f));
	Dressing.Column(FVector(0.f, Side * ColonnadeOffset, 0.f), 104.f, Dressing.Random.FRandRange(380.f, 480.f));
	Ruins.Torch(Radius / 2.f, -Side);

	for (float Angle : {UE_PI / 8.f, UE_PI / 4.f, 3.f * UE_PI / 8.f})
		AddCoin(ArcPoint(Radius, Angle, 95.f), Ruins, ArcYaw(Angle));
}

int32 ARunnerTrackSegment::TrimScenery(TConstArrayView<FRunnerPiece> Pieces)
{
	// Walkable volume of a piece in its own frame: the paved width, from just above the floor up to a canopy-height ceiling.
	constexpr float FloorClearance = 5.f;
	constexpr float ClearHeight = 940.f;
	const float Yaw = GetActorRotation().Yaw;
	TArray<TPair<FTransform, FBox>, TInlineAllocator<8>> Paths;
	for (const FRunnerPiece& Piece : Pieces)
	{
		// Collinear neighbours share this piece's scenery layout; only a turn can carry another path into it.
		if (FMath::Abs(FRotator::NormalizeAxis(Piece.Yaw - Yaw)) < 1.f ||
			FMath::Abs(Piece.StartDistance - StartDistance) > 2.f * RunnerRules::SceneryReach + RunnerRules::SegmentLength) continue;
		const float Length = Piece.Turn ? RunnerRules::CornerSize : RunnerRules::SegmentLength;
		const float HalfWidth = Piece.Turn ? RunnerRules::CornerRadius - 1.f : 560.f;
		Paths.Emplace(FTransform(FRotator(0.f, Piece.Yaw, 0.f), Piece.Location),
			FBox(FVector(1.f, -HalfWidth, FloorClearance), FVector(Length - 1.f, HalfWidth, ClearHeight)));
	}
	if (Paths.IsEmpty()) return 0;

	int32 Removed = 0;
	TInlineComponentArray<UInstancedStaticMeshComponent*> Batches(this);
	for (UInstancedStaticMeshComponent* Batch : Batches)
	{
		const FBox MeshBox = Batch->GetStaticMesh()->GetBounds().GetBox();
		TArray<int32> Intruders;
		for (int32 Index = 0; Index < Batch->GetInstanceCount(); ++Index)
		{
			FTransform Instance;
			Batch->GetInstanceTransform(Index, Instance, true);
			const FBox Box = MeshBox.TransformBy(Instance);
			for (const TPair<FTransform, FBox>& Path : Paths)
			{
				if (Box.InverseTransformBy(Path.Key).Intersect(Path.Value))
				{
					Intruders.Add(Index);					break;
				}
			}
		}
		if (!Intruders.IsEmpty()) Batch->RemoveInstances(Intruders);
		Removed += Intruders.Num();
	}
	return Removed;
}
