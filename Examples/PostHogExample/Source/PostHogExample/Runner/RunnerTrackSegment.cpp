#include "RunnerTrackSegment.h"
#include "RunnerPalette.h"
#include "RunnerRules.h"
#include "RunnerRuins.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Materials/MaterialInstanceDynamic.h"

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

void ARunnerTrackSegment::AddCoin(FVector Position, FRunnerRuins& Ruins)
{
	USphereComponent* Trigger = NewObject<USphereComponent>(this);
	Trigger->SetupAttachment(RootComponent);
	Trigger->SetRelativeLocation(Position);
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
