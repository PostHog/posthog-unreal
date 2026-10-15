#include "RunnerRuins.h"
#include "RunnerPalette.h"
#include "RunnerRules.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	const FLinearColor Sandstone[] = {
		FLinearColor(0.40f, 0.31f, 0.18f), FLinearColor(0.34f, 0.27f, 0.16f),
		FLinearColor(0.46f, 0.37f, 0.22f), FLinearColor(0.26f, 0.27f, 0.26f)};
	const FLinearColor Leaves[] = {
		FLinearColor(0.05f, 0.30f, 0.02f), FLinearColor(0.13f, 0.42f, 0.03f), FLinearColor(0.025f, 0.19f, 0.04f)};
	const FLinearColor Bark[] = {
		FLinearColor(0.13f, 0.08f, 0.045f), FLinearColor(0.19f, 0.13f, 0.075f), FLinearColor(0.09f, 0.07f, 0.05f)};
	const FLinearColor Mortar(0.03f, 0.026f, 0.022f);
	const FLinearColor LeafEdge(0.42f, 0.6f, 0.05f);
	const FLinearColor Rut(0.20f, 0.13f, 0.06f);
	const FLinearColor Cliff(0.07f, 0.08f, 0.06f);
	const FLinearColor Waterfall(0.78f, 0.84f, 0.86f);
	const FLinearColor Moss(0.07f, 0.14f, 0.03f);
	const FLinearColor Groove(0.09f, 0.075f, 0.055f);
	const FLinearColor DeepStone(0.17f, 0.155f, 0.125f);
	const FLinearColor Vine(0.04f, 0.10f, 0.025f);
	const FLinearColor Wood(0.08f, 0.045f, 0.025f);
	const FLinearColor Gold(1.f, 0.62f, 0.12f);
	const FLinearColor Flame(2.4f, 0.7f, 0.06f);
	const FLinearColor Gleam(2.5f, 1.2f, 0.12f);
	const FLinearColor ChasmFloor(0.012f, 0.025f, 0.018f);
	const FLinearColor Earth(0.085f, 0.06f, 0.035f);
	const FLinearColor FarLeaves(0.035f, 0.1f, 0.035f);
	const FLinearColor FarTrunk(0.055f, 0.05f, 0.04f);

	constexpr float SegmentLength = RunnerRules::SegmentLength;
	constexpr float LaneHalf = RunnerRules::LaneWidth / 2.f;
	constexpr float GrooveHalf = 9.f;
	constexpr float PathHalfWidth = RunnerRules::LaneWidth * 1.5f;
	constexpr float EdgeSlabWidth = 100.f;
	constexpr float SlabLength = 200.f;
	constexpr float LedgeY = 660.f;
	constexpr float ColonnadeY = 612.f;
	constexpr float PlinthHeight = 36.f;
	constexpr float PillarStart = 300.f;
	constexpr float PillarSpacing = 600.f;
	constexpr int32 ArchPillarIndex = 3;
	constexpr float ArchX = PillarStart + ArchPillarIndex * PillarSpacing;
	constexpr float PanelLength = 440.f;
	// The jungle floor is a bank beyond a strip of open chasm, so the drop at the path edge stays visible.
	constexpr float BankY = 1150.f;
	constexpr float BankDepth = 2200.f;
	constexpr float BankTop = -120.f;
	// Arch lintels bottom out near 830 cm and the jump apex is lower, so foliage stays clear above this.
	constexpr float CanopyFloor = 950.f;
}

UStaticMeshComponent* RunnerRuins::AttachMesh(AActor* Owner, USceneComponent* Parent, UStaticMesh* Mesh,
	UMaterialInterface* Material, FVector Position, FVector Size, FRotator Rotation, FName CollisionProfile,
	bool bVisible)
{
	UStaticMeshComponent* Component = NewObject<UStaticMeshComponent>(Owner);
	Component->SetupAttachment(Parent);
	Component->SetRelativeLocation(Position);
	Component->SetRelativeRotation(Rotation);
	Component->SetRelativeScale3D(Size / 100.f);
	Component->SetStaticMesh(Mesh);
	Component->SetMaterial(0, Material);
	Component->SetCollisionProfileName(CollisionProfile);
	Component->SetVisibility(bVisible);
	Component->RegisterComponent();
	Owner->AddInstanceComponent(Component);
	return Component;
}

FRunnerRuins::FRunnerRuins(AActor* InOwner, URunnerPaletteSubsystem& InPalette, int32 Seed)
	: Owner(InOwner), Palette(InPalette), Random(Seed)
{
}

void FRunnerRuins::Add(UStaticMesh* Mesh, FLinearColor Color, FVector Center, FVector Size, FRotator Rotation)
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
	Batch->AddInstance(FTransform(Rotation, Center, Size / 100.f));
}

void FRunnerRuins::Box(FLinearColor Color, FVector Center, FVector Size, FRotator Rotation)
{
	Add(Palette.Cube, Color, Center, Size, Rotation);
}

void FRunnerRuins::Attach(USceneComponent* Parent, UStaticMesh* Mesh, UMaterialInstanceDynamic* Material,
	FVector Position, FVector Size, FRotator Rotation)
{
	RunnerRuins::AttachMesh(Owner, Parent, Mesh, Material, Position, Size, Rotation,
		UCollisionProfile::NoCollision_ProfileName);
}

void FRunnerRuins::Path()
{
	// Grooves between lane slabs mark the lane edges.
	for (int32 Lane = -1; Lane <= 1; ++Lane)
	{
		const float Y = RunnerRules::LaneY(Lane);
		const float Stagger = Lane == 0 ? SlabLength / 2.f : 0.f;
		for (float X = -Stagger; X < SegmentLength; X += SlabLength)
			Slab(FMath::Max(X, 0.f), FMath::Min(X + SlabLength, SegmentLength), Y - (LaneHalf - GrooveHalf),
				Y + (LaneHalf - GrooveHalf), false);
	}
	for (float Side : {-1.f, 1.f})
	{
		const float Inner = Side * PathHalfWidth;
		const float Outer = Inner + Side * EdgeSlabWidth;
		for (float X = 0.f; X < SegmentLength;)
		{
			const float Next = FMath::Min(SegmentLength, X + Random.FRandRange(90.f, 170.f));
			Slab(X, Next, FMath::Min(Inner, Outer), FMath::Max(Inner, Outer), true);
			X = Next;
		}
		Box(Rut, FVector(SegmentLength / 2.f, Side * LaneHalf, 0.f), FVector(SegmentLength, 18.f, 1.f));
		Box(Rut, FVector(SegmentLength / 2.f, Inner, 0.f), FVector(SegmentLength, 10.f, 1.f));
	}
}

void FRunnerRuins::Slab(float X0, float X1, float Y0, float Y1, bool bEdge)
{
	if (!bEdge && Random.FRand() < 0.15f) return;
	FVector Size(X1 - X0 - 3.f, Y1 - Y0 - 3.f, 10.f);
	FRotator Tilt(Jitter(0.4f), 0.f, Jitter(0.4f));
	float Top = Random.FRandRange(0.3f, 3.f);
	if (bEdge)
	{
		const float Roll = Random.FRand();
		if (Roll < 0.12f) return;
		if (Roll < 0.3f)
		{
			Tilt.Roll = Jitter(9.f);
			Top -= 4.f;
		}
		else if (Roll < 0.4f) Size.X *= Random.FRandRange(0.45f, 0.7f);
	}
	const FVector Center((X0 + X1) / 2.f, (Y0 + Y1) / 2.f, Top - Size.Z / 2.f);
	Box(Pick(Sandstone), Center, Size, Tilt);
	if (bEdge && Random.FRand() < 0.3f)
	{
		const float Patch = Random.FRandRange(40.f, 90.f);
		Add(Palette.Sphere, Moss, Center + FVector(0.f, 0.f, Size.Z / 2.f), FVector(Patch, Patch * 0.8f, 6.f), Tilt);
	}
	if (!bEdge && Random.FRand() < 0.3f)
	{
		Box(Mortar, Center + FVector(Jitter(40.f), Jitter(60.f), Size.Z / 2.f + 0.3f),
			FVector(Random.FRandRange(40.f, 110.f), 2.5f, 2.f), FRotator(0.f, Jitter(60.f), 0.f));
	}
}

void FRunnerRuins::Edges(bool bArch)
{
	Box(DeepStone, FVector(SegmentLength / 2.f, 0.f, -355.f), FVector(SegmentLength, 2.f * LedgeY, 700.f));
	for (float Side : {-1.f, 1.f})
	{
		for (float Z : {-140.f, -330.f, -520.f})
			Box(Mortar, FVector(SegmentLength / 2.f, Side * (LedgeY + 0.5f), Z), FVector(SegmentLength, 2.f, 4.f));
		for (float X = 0.f; X < SegmentLength;)
		{
			const float Piece = Random.FRandRange(80.f, 240.f);
			const float Height = Random.FRandRange(14.f, 38.f);
			if (Random.FRand() < 0.85f)
				Box(Pick(Sandstone), FVector(X + Piece / 2.f, Side * 537.f, Height / 2.f - 2.f), FVector(Piece, 26.f, Height),
					FRotator(0.f, Jitter(1.5f), Jitter(3.f)));
			X += Piece + Random.FRandRange(15.f, 70.f);
		}
		for (int32 Index = 0; PillarStart + Index * PillarSpacing < SegmentLength; ++Index)
		{
			if (bArch && Index == ArchPillarIndex) continue;
			Pillar(PillarStart + Index * PillarSpacing, Side, Random.FRandRange(430.f, 520.f), Random.FRand() < 0.3f);
		}
		for (float X = PillarSpacing; X < SegmentLength; X += PillarSpacing)
		{
			if (Random.FRand() < 0.5f) CarvedPanel(X, Side);
			else Vines(FVector(X, Side * (LedgeY + 4.f), 0.f), FVector(200.f, 0.f, 0.f), Random.RandRange(2, 5), 120.f, 420.f);
		}
		for (float X = Random.FRandRange(50.f, 250.f); X < SegmentLength; X += Random.FRandRange(250.f, 500.f))
			Ferns(FVector(X, Side * Random.FRandRange(560.f, 600.f), 0.f));
		for (int32 Index = 0; Index < 3; ++Index)
			Foliage(FVector(Random.FRandRange(0.f, SegmentLength), Side * Random.FRandRange(640.f, 690.f), 5.f), Random.FRandRange(60.f, 140.f));
		Undergrowth(Side);
		Jungle(Side);
	}
	Overhang();
	if (bArch) Arch();
}

float FRunnerRuins::Column(FVector Base, float PlinthWidth, float Diameter, float Height)
{
	const float Top = PlinthHeight + Height;
	const float CapitalWidth = PlinthWidth + 6.f;
	Box(Pick(Sandstone), Base + FVector(0.f, 0.f, PlinthHeight / 2.f), FVector(PlinthWidth, PlinthWidth, PlinthHeight));
	Add(Palette.Cylinder, Pick(Sandstone), Base + FVector(0.f, 0.f, PlinthHeight + Height / 2.f), FVector(Diameter, Diameter, Height));
	Box(Pick(Sandstone), Base + FVector(0.f, 0.f, Top + 15.f), FVector(CapitalWidth, CapitalWidth, 30.f));
	return Top;
}

void FRunnerRuins::Rubble(FVector Center, float MinChunk, float MaxChunk)
{
	const float Chunk = Random.FRandRange(MinChunk, MaxChunk);
	Box(Pick(Sandstone), Center + FVector(0.f, 0.f, Chunk / 2.f), FVector(Chunk), FRotator(0.f, Random.FRandRange(0.f, 90.f), 0.f));
}

void FRunnerRuins::Pillar(float X, float Side, float Height, bool bBroken)
{
	const FVector Base(X, Side * ColonnadeY, 0.f);
	if (bBroken)
	{
		Box(Pick(Sandstone), Base + FVector(0.f, 0.f, PlinthHeight / 2.f), FVector(104.f, 104.f, PlinthHeight));
		const float Stump = Random.FRandRange(110.f, 280.f);
		Add(Palette.Cylinder, Pick(Sandstone), Base + FVector(0.f, 0.f, PlinthHeight + Stump / 2.f), FVector(80.f, 80.f, Stump),
			FRotator(Jitter(5.f), 0.f, Jitter(5.f)));
		const FVector Capital = Base + FVector(Jitter(160.f), Side * 25.f, 15.f);
		Box(Pick(Sandstone), Capital, FVector(110.f, 110.f, 30.f), FRotator(Jitter(8.f), Random.FRandRange(0.f, 90.f), 0.f));
		Add(Palette.Sphere, Moss, Capital + FVector(0.f, 0.f, 16.f), FVector(80.f, 70.f, 8.f));
	}
	else
	{
		const float Top = Column(Base, 104.f, 80.f, Height);
		for (float Fraction : {0.33f, 0.66f})
			Add(Palette.Cylinder, Mortar, Base + FVector(0.f, 0.f, PlinthHeight + Height * Fraction), FVector(82.f, 82.f, 3.f));
		if (Random.FRand() < 0.5f) Box(Moss, Base + FVector(0.f, 0.f, Top + 31.f), FVector(90.f, 80.f, 4.f));
		if (Random.FRand() < 0.7f)
			Vines(Base + FVector(0.f, -Side * 43.f, Top), FVector(25.f, 0.f, 0.f), Random.RandRange(1, 3), 80.f, Height * 0.7f);
		if (Random.FRand() < 0.3f) Foliage(Base + FVector(0.f, 0.f, Top + 35.f), Random.FRandRange(60.f, 100.f));
	}
	if (Random.FRand() < 0.6f)
		Foliage(Base + FVector(Jitter(60.f), Side * Random.FRandRange(0.f, 40.f), 10.f), Random.FRandRange(50.f, 90.f));
}

void FRunnerRuins::CarvedPanel(float X, float Side)
{
	const float Height = Random.FRandRange(150.f, 260.f);
	const FVector Center(X, Side * (ColonnadeY + 10.f), Height / 2.f);
	Box(Pick(Sandstone), Center, FVector(PanelLength, 34.f, Height));
	Box(Moss, Center + FVector(Jitter(80.f), 0.f, Height / 2.f + 2.f), FVector(Random.FRandRange(150.f, 330.f), 36.f, 4.f));
	const int32 Count = Random.RandRange(2, 3);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Offset = -PanelLength / 2.f + (Index + 0.5f) * PanelLength / Count;
		Inset(Center + FVector(Offset, -Side * 17.f, 0.f), FVector(0.f, -Side, 0.f), PanelLength / Count - 50.f, Height - 60.f);
	}
	if (Random.FRand() < 0.5f)
		Box(Pick(Sandstone), Center + FVector(Jitter(150.f), 0.f, Height / 2.f + 12.f), FVector(Random.FRandRange(40.f, 90.f), 34.f, 24.f));
	if (Random.FRand() < 0.6f)
		Foliage(Center + FVector(Jitter(150.f), 0.f, Height / 2.f + 10.f), Random.FRandRange(60.f, 100.f));
	Vines(FVector(X, Side * (ColonnadeY - 10.f), Height), FVector(PanelLength * 0.4f, 0.f, 0.f), Random.RandRange(1, 4), 40.f, Height * 0.7f);
}

void FRunnerRuins::Inset(FVector FaceCenter, FVector Normal, float Width, float Height)
{
	const FVector Depth = Normal.GetAbs();
	const FVector Across = FVector(1.f, 1.f, 0.f) - Depth;
	Box(Groove, FaceCenter + Normal, Depth * 2.f + Across * Width + FVector(0.f, 0.f, Height));
	Box(Pick(Sandstone), FaceCenter + Normal * 2.f, Depth * 3.f + Across * Width * 0.4f + FVector(0.f, 0.f, Height * 0.3f));
	Box(Pick(Sandstone), FaceCenter + Normal * 2.f + FVector(0.f, 0.f, Height * 0.3f), Depth * 3.f + Across * Width * 0.7f + FVector(0.f, 0.f, Height * 0.1f));
}

void FRunnerRuins::Arch()
{
	const float Height = 760.f;
	float Top = 0.f;
	for (float Side : {-1.f, 1.f})
	{
		const FVector Base(ArchX, Side * ColonnadeY, 0.f);
		Top = Column(Base, 110.f, 86.f, Height);
		Foliage(Base + FVector(0.f, Side * 20.f, 10.f), 90.f);
	}
	const FVector Lintel(ArchX, 0.f, Top + 75.f);
	Box(Pick(Sandstone), Lintel, FVector(110.f, 2.f * LedgeY + 60.f, 90.f));
	Box(Groove, Lintel + FVector(-56.f, 0.f, 22.f), FVector(2.f, 2.f * LedgeY + 40.f, 12.f));
	Inset(Lintel + FVector(-55.f, 0.f, -8.f), FVector(-1.f, 0.f, 0.f), 160.f, 50.f);
	Box(Pick(Sandstone), Lintel + FVector(0.f, 0.f, 75.f), FVector(90.f, 130.f, 60.f));
	Vines(Lintel + FVector(-40.f, 0.f, -45.f), FVector(0.f, 600.f, 0.f), 8, 60.f, 260.f);
	for (int32 Index = 0; Index < 2; ++Index)
		Foliage(Lintel + FVector(0.f, Jitter(500.f), 50.f), Random.FRandRange(70.f, 120.f));
}

void FRunnerRuins::Vines(FVector Top, FVector Spread, int32 Count, float MinLength, float MaxLength)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const float Length = Random.FRandRange(MinLength, FMath::Max(MinLength, MaxLength));
		Add(Palette.Cylinder, Vine, Top + Spread * Jitter(1.f) - FVector(0.f, 0.f, Length / 2.f),
			FVector(7.f, 7.f, Length), FRotator(Jitter(3.f), 0.f, Jitter(3.f)));
	}
	Foliage(Top, 35.f);
}

void FRunnerRuins::Foliage(FVector Center, float Size)
{
	const int32 Lobes = Random.RandRange(2, 4);
	for (int32 Index = 0; Index < Lobes; ++Index)
	{
		const FVector Offset(Jitter(0.4f) * Size, Jitter(0.4f) * Size, Random.FRandRange(0.f, 0.2f) * Size);
		const float Lobe = Size * Random.FRandRange(0.6f, 1.f);
		Add(Palette.Sphere, Pick(Leaves), Center + Offset, FVector(Lobe, Lobe, Lobe * 0.55f), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
	}
	const float Edge = Size * 0.45f;
	Add(Palette.Sphere, LeafEdge, Center + FVector(Jitter(0.4f) * Size, Jitter(0.4f) * Size, Size * 0.25f),
		FVector(Edge, Edge * 0.5f, Edge * 0.3f), FRotator(Jitter(20.f), Random.FRandRange(0.f, 360.f), 0.f));
	if (Random.FRand() < 0.35f)
		Add(Palette.Cone, Pick(Leaves), Center + FVector(0.f, 0.f, Size * 0.35f), FVector(Size * 0.5f, Size * 0.5f, Size * 0.7f));
}

void FRunnerRuins::Ferns(FVector Center)
{
	for (int32 Index = 0, Count = Random.RandRange(3, 5); Index < Count; ++Index)
	{
		const float Frond = Random.FRandRange(40.f, 75.f);
		Add(Palette.Sphere, Index == 0 ? LeafEdge : Pick(Leaves), Center + FVector(0.f, 0.f, Frond * 0.2f), FVector(Frond * 1.4f, Frond * 0.35f, Frond * 0.12f),
			FRotator(Random.FRandRange(15.f, 35.f), Index * 360.f / Count + Jitter(15.f), 0.f));
	}
}

void FRunnerRuins::Chasm()
{
	Box(ChasmFloor, FVector(SegmentLength / 2.f, 0.f, -2550.f), FVector(SegmentLength, 120000.f, 100.f));
	Box(DeepStone, FVector(SegmentLength / 2.f, 0.f, -1600.f), FVector(520.f, 860.f, 1800.f));
}

void FRunnerRuins::MistTower(float X, float Side, float Height)
{
	if (Random.FRand() < 0.2f)
	{
		const float Width = Random.FRandRange(1200.f, 2400.f);
		const float Y = Side * Random.FRandRange(13000.f, 20000.f);
		Box(Cliff, FVector(X, Y, -900.f), FVector(Width, 500.f, Height + 2600.f));
		Box(Waterfall, FVector(X + Jitter(0.3f) * Width, Y - Side * 251.f, -900.f), FVector(Random.FRandRange(60.f, 140.f), 4.f, Height + 2600.f));
		const float Rim = 400.f + Height / 2.f;
		for (int32 Index = 0; Index < 3; ++Index)
		{
			const float Lobe = Random.FRandRange(500.f, 900.f);
			Add(Palette.Sphere, FarLeaves, FVector(X + Jitter(0.4f) * Width, Y + Jitter(150.f), Rim + Random.FRandRange(0.f, 120.f)),
				FVector(Lobe, Random.FRandRange(500.f, 800.f), Lobe * 0.4f));
		}
		return;
	}
	if (Random.FRand() < 0.15f) return;
	const FVector Base(X + Jitter(200.f), Side * Random.FRandRange(4500.f, 9000.f), -2500.f);
	const float Top = Height + 300.f;
	for (int32 Index = 0, Count = Random.RandRange(2, 3); Index < Count; ++Index)
	{
		const float Trunk = Random.FRandRange(90.f, 180.f);
		Add(Palette.Cylinder, FarTrunk, Base + FVector(Jitter(300.f), Jitter(300.f), (Top - Base.Z) / 2.f),
			FVector(Trunk, Trunk, Top - Base.Z), FRotator(Jitter(3.f), 0.f, Jitter(3.f)));
	}
	for (int32 Index = 0, Count = Random.RandRange(3, 5); Index < Count; ++Index)
	{
		const float Lobe = Random.FRandRange(500.f, 1100.f);
		Add(Palette.Sphere, FarLeaves, FVector(Base.X + Jitter(450.f), Base.Y + Jitter(450.f), Top + Random.FRandRange(-250.f, 150.f)),
			FVector(Lobe, Lobe * Random.FRandRange(0.8f, 1.1f), Lobe * 0.5f), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
	}
}

void FRunnerRuins::Statue(float X, float Side, bool bSeated)
{
	const float Y = Side * 860.f;
	const float Front = -Side;
	const FLinearColor Color = Pick(Sandstone);
	Box(DeepStone, FVector(X, Y, -450.f), FVector(520.f, 420.f, 900.f));
	Box(Color, FVector(X, Y, 25.f), FVector(300.f, 300.f, 50.f));
	float Crown;
	if (bSeated)
	{
		Box(Color, FVector(X, Y + Front * 40.f, 95.f), FVector(170.f, 170.f, 90.f));
		Box(Color, FVector(X, Y - Front * 30.f, 230.f), FVector(150.f, 100.f, 190.f));
		Box(Color, FVector(X, Y - Front * 30.f, 335.f), FVector(200.f, 90.f, 40.f));
		for (float Arm : {-1.f, 1.f})
		{
			Box(Color, FVector(X + Arm * 90.f, Y - Front * 20.f, 265.f), FVector(34.f, 40.f, 130.f));
			Box(Color, FVector(X + Arm * 80.f, Y + Front * 45.f, 160.f), FVector(34.f, 120.f, 30.f));
			Box(Mortar, FVector(X + Arm * 18.f, Y + Front * 18.f, 410.f), FVector(14.f, 4.f, 7.f));
		}
		Add(Palette.Sphere, Color, FVector(X, Y - Front * 25.f, 400.f), FVector(92.f, 88.f, 100.f));
		Add(Palette.Cone, Color, FVector(X, Y - Front * 25.f, 485.f), FVector(120.f, 120.f, 90.f));
		Crown = 530.f;
	}
	else
	{
		Box(Color, FVector(X, Y, 190.f), FVector(220.f, 200.f, 280.f));
		Box(Color, FVector(X, Y + Front * 112.f, 250.f), FVector(200.f, 26.f, 30.f));
		Box(Color, FVector(X, Y + Front * 115.f, 170.f), FVector(36.f, 30.f, 70.f));
		Box(Mortar, FVector(X, Y + Front * 101.f, 105.f), FVector(110.f, 4.f, 22.f));
		for (float Eye : {-1.f, 1.f})
		{
			Box(Mortar, FVector(X + Eye * 50.f, Y + Front * 101.f, 215.f), FVector(50.f, 4.f, 26.f));
			Box(Color, FVector(X + Eye * 125.f, Y, 200.f), FVector(30.f, 80.f, 110.f));
		}
		Box(Color, FVector(X, Y, 350.f), FVector(260.f, 220.f, 40.f));
		Add(Palette.Cone, Color, FVector(X, Y, 420.f), FVector(200.f, 200.f, 100.f));
		Crown = 400.f;
	}
	Foliage(FVector(X + Jitter(120.f), Y, 60.f), 110.f);
	Vines(FVector(X + Jitter(60.f), Y + Front * 50.f, Crown - 40.f), FVector(60.f, 0.f, 0.f), 3, 80.f, 250.f);
}

void FRunnerRuins::Torch(float X, float Side)
{
	const FVector Base(X, Side * 575.f, 0.f);
	Add(Palette.Cylinder, Wood, Base + FVector(0.f, 0.f, 115.f), FVector(12.f, 12.f, 230.f));
	Add(Palette.Cone, Wood, Base + FVector(0.f, 0.f, 240.f), FVector(40.f, 40.f, 26.f), FRotator(180.f, 0.f, 0.f));
	Attach(Owner->GetRootComponent(), Palette.Sphere, Palette.Glow(Flame), Base + FVector(0.f, 0.f, 258.f), FVector(24.f, 24.f, 30.f));
	Attach(Owner->GetRootComponent(), Palette.Cone, Palette.Glow(Flame), Base + FVector(0.f, 0.f, 280.f), FVector(18.f, 18.f, 30.f));
	UPointLightComponent* Light = NewObject<UPointLightComponent>(Owner);
	Light->SetupAttachment(Owner->GetRootComponent());
	Light->SetRelativeLocation(Base + FVector(0.f, -Side * 40.f, 280.f));
	Light->SetIntensityUnits(ELightUnits::Candelas);
	Light->SetIntensity(60.f);
	Light->SetAttenuationRadius(1400.f);
	Light->SetLightColor(FLinearColor(1.f, 0.5f, 0.18f));
	Light->SetCastShadows(false);
	Light->RegisterComponent();
	Owner->AddInstanceComponent(Light);
}

void FRunnerRuins::FallenPillar(FVector Center)
{
	const FRotator Lying(0.f, 0.f, 90.f);
	Add(Palette.Cylinder, Pick(Sandstone), Center, FVector(80.f, 80.f, 226.f), Lying);
	for (float Joint : {-55.f, 40.f})
		Add(Palette.Cylinder, Mortar, Center + FVector(0.f, Joint, 0.f), FVector(82.f, 82.f, 3.f), Lying);
	Rubble(FVector(Center.X + 75.f, Center.Y + Jitter(80.f), 0.f), 30.f, 30.f);
	if (Random.FRand() < 0.6f) Foliage(Center + FVector(0.f, Jitter(60.f), 38.f), 40.f);
}

void FRunnerRuins::CrackedPosts(FVector BeamCenter)
{
	for (float Offset : {-RunnerRuins::PostOffset, RunnerRuins::PostOffset})
	{
		const FVector Face = BeamCenter + FVector(-RunnerRuins::HazardHalfDepth, Offset, 0.f);
		Box(Mortar, FVector(Face.X, Face.Y, Random.FRandRange(10.f, 70.f)), FVector(2.f, 4.f, Random.FRandRange(30.f, 70.f)),
			FRotator(0.f, 0.f, Jitter(25.f)));
		Rubble(FVector(BeamCenter.X + Random.FRandRange(-20.f, 60.f), BeamCenter.Y + Offset * 1.1f, 0.f), 18.f, 30.f);
	}
}

void FRunnerRuins::CarvedLintel(FVector Center)
{
	Box(Pick(Sandstone), Center + FVector(0.f, 0.f, 63.f), FVector(116.f, 280.f, 16.f), FRotator(0.f, 0.f, Jitter(3.f)));
	Inset(Center + FVector(-50.f, 0.f, 0.f), FVector(-1.f, 0.f, 0.f), 170.f, 60.f);
	Vines(Center + FVector(-48.f, 0.f, -55.f), FVector(0.f, 90.f, 0.f), 3, 10.f, 25.f);
	if (Random.FRand() < 0.7f) Foliage(Center + FVector(0.f, Jitter(80.f), 72.f), 50.f);
}

void FRunnerRuins::BlockStack(FVector Center)
{
	const float Face = Center.X - RunnerRuins::HazardHalfDepth;
	const float Courses[][2] = {{-40.f, 70.f}, {-95.f, 15.f}, {-50.f, 60.f}};
	for (int32 Course = 0; Course < 3; ++Course)
	{
		const float Z = 40.f + Course * 80.f;
		if (Course > 0) Box(Mortar, FVector(Face, Center.Y, Z - 40.f), FVector(2.f, 220.f, 3.f));
		for (float Joint : Courses[Course])
			Box(Mortar, FVector(Face, Center.Y + Joint, Z), FVector(2.f, 3.f, 80.f));
	}
	Inset(FVector(Face + 1.f, Center.Y + 10.f, 120.f), FVector(-1.f, 0.f, 0.f), 70.f, 60.f);
	for (int32 Index = 0, Count = Random.RandRange(2, 3); Index < Count; ++Index)
	{
		const float Height = Random.FRandRange(30.f, 60.f);
		Box(Pick(Sandstone), Center + FVector(Jitter(15.f), Jitter(75.f), 120.f + Height / 2.f),
			FVector(Random.FRandRange(50.f, 90.f), Random.FRandRange(50.f, 90.f), Height),
			FRotator(0.f, Jitter(15.f), Jitter(4.f)));
	}
	for (int32 Index = 0; Index < 2; ++Index)
		Rubble(FVector(Face - Random.FRandRange(10.f, 50.f), Center.Y + Jitter(90.f), 0.f), 18.f, 32.f);
	if (Random.FRand() < 0.7f) Foliage(Center + FVector(0.f, Jitter(60.f), 135.f), 60.f);
}

void FRunnerRuins::Relic(USceneComponent* Trigger)
{
	UMaterialInstanceDynamic* Metal = Palette.Paint(Gold);
	Attach(Trigger, Palette.Sphere, Metal, FVector::ZeroVector, FVector(12.f, 46.f, 46.f));
	Attach(Trigger, Palette.Sphere, Palette.Glow(Gleam), FVector(-4.f, 0.f, 0.f), FVector(10.f, 18.f, 18.f));
	Attach(Trigger, Palette.Sphere, Metal, FVector(0.f, 0.f, 30.f), FVector(14.f, 18.f, 18.f));
}

void FRunnerRuins::Undergrowth(float Side)
{
	for (float X = Random.FRandRange(0.f, 120.f); X < SegmentLength; X += Random.FRandRange(110.f, 210.f))
		BroadLeaves(FVector(X, Side * Random.FRandRange(640.f, 700.f), Random.FRandRange(-15.f, 15.f)), Side);
	for (float X = Random.FRandRange(0.f, 300.f); X < SegmentLength; X += Random.FRandRange(220.f, 420.f))
	{
		const float Height = Random.FRandRange(60.f, 260.f);
		Box(Moss, FVector(X, Side * (LedgeY + 1.5f), -Height / 2.f - Random.FRandRange(0.f, 60.f)),
			FVector(Random.FRandRange(70.f, 220.f), 3.f, Height));
	}
}

void FRunnerRuins::BroadLeaves(FVector Center, float Side)
{
	for (int32 Index = 0, Count = Random.RandRange(2, 4); Index < Count; ++Index)
	{
		const float Leaf = Random.FRandRange(80.f, 160.f);
		// Leaves fan outward over the drop and droop at the tips.
		const float Yaw = Side * 90.f + Random.FRandRange(-70.f, 70.f);
		const FRotator Rotation(Random.FRandRange(-30.f, 10.f), Yaw, Jitter(20.f));
		const FVector Offset = Rotation.RotateVector(FVector(Leaf * 0.4f, 0.f, 0.f));
		Add(Palette.Sphere, Random.FRand() < 0.2f ? LeafEdge : Pick(Leaves), Center + Offset + FVector(0.f, 0.f, Leaf * 0.15f),
			FVector(Leaf, Leaf * 0.38f, 6.f), Rotation);
	}
}

void FRunnerRuins::Jungle(float Side)
{
	Box(Earth, FVector(SegmentLength / 2.f, Side * (BankY + BankDepth / 2.f), BankTop - 700.f),
		FVector(SegmentLength + 4.f, BankDepth, 1400.f));
	for (float X = Random.FRandRange(0.f, 200.f); X < SegmentLength; X += Random.FRandRange(300.f, 500.f))
		Vines(FVector(X, Side * (BankY - 6.f), BankTop + 10.f), FVector(160.f, 0.f, 0.f), Random.RandRange(2, 4), 150.f, 600.f);

	bool bNearRow = Random.FRand() < 0.5f;
	for (float X = Random.FRandRange(0.f, 150.f); X < SegmentLength; X += Random.FRandRange(190.f, 320.f))
	{
		bNearRow = !bNearRow;
		const float Y = bNearRow ? Random.FRandRange(BankY + 90.f, BankY + 450.f) : Random.FRandRange(BankY + 550.f, BankY + 1800.f);
		Tree(FVector(X, Side * Y, BankTop), Random.FRandRange(1500.f, 2500.f),
			bNearRow ? Random.FRandRange(100.f, 170.f) : Random.FRandRange(70.f, 130.f));
	}
	// Understory masses close the gaps between trunks at eye height.
	for (float X = Random.FRandRange(0.f, 150.f); X < SegmentLength; X += Random.FRandRange(170.f, 280.f))
		Canopy(FVector(X, Side * Random.FRandRange(BankY + 150.f, BankY + 1300.f), Random.FRandRange(150.f, 650.f)),
			Random.FRandRange(260.f, 460.f), BankTop + 20.f);
	for (float X = Random.FRandRange(0.f, 100.f); X < SegmentLength; X += Random.FRandRange(130.f, 230.f))
		Foliage(FVector(X, Side * Random.FRandRange(BankY + 30.f, BankY + 380.f), BankTop + 15.f), Random.FRandRange(90.f, 190.f));
	for (float X = Random.FRandRange(0.f, 300.f); X < SegmentLength; X += Random.FRandRange(300.f, 600.f))
		Ferns(FVector(X, Side * Random.FRandRange(BankY + 20.f, BankY + 200.f), BankTop));
	for (float X = Random.FRandRange(0.f, 200.f); X < SegmentLength; X += Random.FRandRange(250.f, 450.f))
		Add(Palette.Sphere, Moss, FVector(X, Side * Random.FRandRange(BankY + 100.f, BankY + 1900.f), BankTop),
			FVector(Random.FRandRange(200.f, 420.f), Random.FRandRange(150.f, 300.f), 14.f), FRotator(0.f, Random.FRandRange(0.f, 180.f), 0.f));
	Log(FVector(Random.FRandRange(300.f, SegmentLength - 300.f), Side * Random.FRandRange(BankY + 150.f, BankY + 450.f), BankTop + 30.f),
		Random.FRandRange(260.f, 460.f), Jitter(35.f));
	for (float X = Random.FRandRange(0.f, 200.f); X < SegmentLength; X += Random.FRandRange(260.f, 420.f))
		Canopy(FVector(X, Side * Random.FRandRange(750.f, 2400.f), Random.FRandRange(1300.f, 2100.f)), Random.FRandRange(320.f, 540.f), CanopyFloor);
	// Hazy back wall of foliage behind the trunks keeps the sides closed.
	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float Mass = Random.FRandRange(1000.f, 1600.f);
		Add(Palette.Sphere, FarLeaves, FVector((Index + Random.FRand()) * SegmentLength / 3.f, Side * Random.FRandRange(3400.f, 4200.f),
			Random.FRandRange(300.f, 1400.f)), FVector(Mass, Random.FRandRange(700.f, 1000.f), Mass * Random.FRandRange(0.7f, 1.f)));
	}
}

void FRunnerRuins::Tree(FVector Base, float Height, float Diameter)
{
	const FRotator Lean(Jitter(4.f), 0.f, Jitter(4.f));
	const FVector Up = Lean.RotateVector(FVector::UpVector);
	Add(Palette.Cylinder, Pick(Bark), Base + Up * (Height / 2.f), FVector(Diameter, Diameter, Height), Lean);
	Add(Palette.Cone, Bark[0], Base + FVector(0.f, 0.f, Diameter * 0.6f), FVector(Diameter * 2.4f, Diameter * 2.4f, Diameter * 1.4f));
	if (Random.FRand() < 0.4f)
	{
		const float Band = Random.FRandRange(60.f, 160.f);
		Add(Palette.Cylinder, Moss, Base + Up * Random.FRandRange(Band, Height * 0.4f), FVector(Diameter + 4.f, Diameter + 4.f, Band), Lean);
	}
	Canopy(Base + Up * Height + FVector(0.f, 0.f, Random.FRandRange(-60.f, 60.f)), Random.FRandRange(450.f, 720.f), CanopyFloor);
}

void FRunnerRuins::Canopy(FVector Center, float Size, float MinBottom)
{
	Center.Z = FMath::Max(Center.Z, MinBottom + Size * 0.3f);
	for (int32 Index = 0, Lobes = Random.RandRange(3, 5); Index < Lobes; ++Index)
	{
		const float Lobe = Size * Random.FRandRange(0.55f, 1.f);
		const float Thickness = Lobe * Random.FRandRange(0.5f, 0.75f);
		FVector Position = Center + FVector(Jitter(0.6f) * Size, Jitter(0.6f) * Size, Jitter(0.15f) * Size);
		Position.Z = FMath::Max(Position.Z, MinBottom + Thickness / 2.f);
		Add(Palette.Sphere, Pick(Leaves), Position, FVector(Lobe, Lobe * Random.FRandRange(0.8f, 1.1f), Thickness),
			FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
	}
	Add(Palette.Sphere, LeafEdge, Center + FVector(Jitter(0.3f) * Size, Jitter(0.3f) * Size, Size * 0.2f),
		FVector(Size * 0.5f, Size * 0.4f, Size * 0.16f), FRotator(0.f, Random.FRandRange(0.f, 360.f), 0.f));
	if (Random.FRand() < 0.3f)
		Add(Palette.Cone, Pick(Leaves), Center + FVector(0.f, 0.f, Size * 0.3f), FVector(Size * 0.6f, Size * 0.6f, Size * 0.5f));
}

void FRunnerRuins::Bough(FVector Root, FVector Tip)
{
	const FVector Along = Tip - Root;
	Add(Palette.Cylinder, Pick(Bark), (Root + Tip) / 2.f, FVector(45.f, 45.f, Along.Size()),
		FRotationMatrix::MakeFromZ(Along.GetSafeNormal()).Rotator());
}

void FRunnerRuins::Overhang()
{
	for (int32 Index = 0, Count = Random.RandRange(2, 3); Index < Count; ++Index)
	{
		const float Side = Random.FRand() < 0.5f ? -1.f : 1.f;
		const float X = (Index + Random.FRandRange(0.2f, 0.8f)) * SegmentLength / Count;
		const float RootY = Random.FRandRange(BankY + 200.f, BankY + 500.f);
		const float RootZ = Random.FRandRange(1500.f, 1900.f);
		Tree(FVector(X, Side * RootY, BankTop), RootZ - BankTop + Random.FRandRange(200.f, 500.f), Random.FRandRange(100.f, 140.f));
		const FVector Root(X, Side * RootY, RootZ);
		const FVector Tip(X + Jitter(300.f), Side * Random.FRandRange(-250.f, 350.f), Random.FRandRange(1200.f, 1400.f));
		Bough(Root, Tip);
		Canopy((Root + Tip) / 2.f, Random.FRandRange(380.f, 520.f), CanopyFloor);
		Canopy(Tip, Random.FRandRange(420.f, 600.f), CanopyFloor);
		// Vines over the lanes stop well above the jump apex so they never read as an obstacle.
		const float Hang = CanopyFloor + 20.f;
		Vines(FVector(Tip.X, Tip.Y, Hang), FVector(150.f, 150.f, 0.f), Random.RandRange(3, 6), 100.f, Hang - 640.f);
	}
}

void FRunnerRuins::Log(FVector Center, float Length, float Yaw)
{
	const float Diameter = Random.FRandRange(55.f, 85.f);
	const FRotator Lying(90.f, Yaw, 0.f);
	Add(Palette.Cylinder, Pick(Bark), Center + FVector(0.f, 0.f, Diameter / 2.f - 10.f), FVector(Diameter, Diameter, Length), Lying);
	Add(Palette.Sphere, Moss, Center + FVector(0.f, 0.f, Diameter - 12.f), FVector(Length * 0.5f, Diameter * 0.7f, 10.f),
		FRotator(0.f, Yaw, 0.f));
	Ferns(Center + Lying.RotateVector(FVector(0.f, 0.f, Length * 0.45f)));
}
