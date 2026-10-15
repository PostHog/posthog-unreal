#include "RunnerTrackSegment.h"
#include "RunnerRules.h"
#include "Components/SceneComponent.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "UObject/ConstructorHelpers.h"

ARunnerTrackSegment::ARunnerTrackSegment()
{
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeAsset(TEXT("/Engine/BasicShapes/Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereAsset(TEXT("/Engine/BasicShapes/Sphere"));
	Cube = CubeAsset.Object;
	Sphere = SphereAsset.Object;
}

UMaterialInstanceDynamic* ARunnerTrackSegment::MakeMaterial(FLinearColor Color)
{
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial"));
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(Base, this);
	Material->SetVectorParameterValue(TEXT("Color"), Color);
	Materials.Add(Material);
	return Material;
}

UStaticMeshComponent* ARunnerTrackSegment::AddBlock(FVector Position, FVector Size,
	UMaterialInstanceDynamic* Material, bool bCollision)
{
	UStaticMeshComponent* Mesh = NewObject<UStaticMeshComponent>(this);
	Mesh->SetupAttachment(RootComponent);
	Mesh->SetRelativeLocation(Position);
	Mesh->SetRelativeScale3D(Size / 100.f);
	Mesh->SetStaticMesh(Cube);
	Mesh->SetMaterial(0, Material);
	Mesh->SetCollisionProfileName(bCollision ? UCollisionProfile::BlockAll_ProfileName : UCollisionProfile::NoCollision_ProfileName);
	Mesh->RegisterComponent();
	AddInstanceComponent(Mesh);
	return Mesh;
}

void ARunnerTrackSegment::AddCoin(FVector Position, UMaterialInstanceDynamic* Material)
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

	UStaticMeshComponent* Visual = NewObject<UStaticMeshComponent>(this);
	Visual->SetupAttachment(Trigger);
	Visual->SetStaticMesh(Sphere);
	Visual->SetRelativeScale3D(FVector(0.4f, 0.13f, 0.4f));
	Visual->SetMaterial(0, Material);
	Visual->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Visual->RegisterComponent();
	AddInstanceComponent(Visual);
}

void ARunnerTrackSegment::Build(int32 SegmentIndex)
{
	FRandomStream Random(17389 + SegmentIndex);
	UMaterialInstanceDynamic* Road = MakeMaterial(FLinearColor(0.025f, 0.035f, 0.055f));
	UMaterialInstanceDynamic* Cyan = MakeMaterial(FLinearColor(0.02f, 0.7f, 0.85f));
	UMaterialInstanceDynamic* Gold = MakeMaterial(FLinearColor(1.f, 0.65f, 0.03f));
	UMaterialInstanceDynamic* Red = MakeMaterial(FLinearColor(0.9f, 0.07f, 0.12f));
	UMaterialInstanceDynamic* Building = MakeMaterial(FLinearColor(0.055f, 0.08f, 0.13f));

	const float Length = RunnerRules::SegmentLength;
	AddBlock(FVector(Length / 2.f, 0.f, -25.f), FVector(Length, 1100.f, 50.f), Road, true);

	// Dashed lane markings and continuous edge strips make the three paths legible at speed.
	for (float X = 150.f; X < Length; X += 400.f)
	{
		for (float Y : {-150.f, 150.f})
			AddBlock(FVector(X, Y, 1.f), FVector(160.f, 8.f, 2.f), Cyan, false);
	}
	for (float Side : {-1.f, 1.f})
	{
		AddBlock(FVector(Length / 2.f, Side * 525.f, 12.f), FVector(Length, 18.f, 24.f), Cyan, false);
		AddBlock(FVector(Length / 2.f, Side * 650.f, -10.f), FVector(Length, 220.f, 25.f), Building, false);
		for (float X = 400.f; X < Length; X += 800.f)
		{
			const float Height = Random.FRandRange(650.f, 2000.f);
			AddBlock(FVector(X, Side * 1100.f, Height / 2.f), FVector(550.f, 500.f, Height), Building, false);
			for (float Z = 180.f; Z < Height; Z += 240.f)
				AddBlock(FVector(X, Side * 845.f, Z), FVector(350.f, 4.f, 40.f), Cyan, false);
		}
	}

	// Every row has a fully open lane. The opening is also marked by a line of coins.
	const int32 SafeLane = Random.RandRange(-1, 1);
	if (SegmentIndex > 0)
	{
		for (int32 Lane = -1; Lane <= 1; ++Lane)
		{
			if (Lane == SafeLane) continue;
			const int32 Kind = Random.RandRange(0, 2);
			FVector Position(1400.f, RunnerRules::LaneY(Lane), 0.f);
			FVector Size(100.f, 220.f, 240.f);
			UMaterialInstanceDynamic* Material = Red;
			if (Kind == 0) // Jumpable low hurdle.
			{
				Size.Z = 80.f;
				Position.Z = 40.f;
				Material = Gold;
			}
			else if (Kind == 1) // Slide beneath a beam with 130 cm of clearance.
			{
				Size.Z = 110.f;
				Position.Z = 185.f;
				Material = Cyan;
				for (float Offset : {-125.f, 125.f})
				{
					UStaticMeshComponent* Post = AddBlock(Position + FVector(0.f, Offset, -65.f),
						FVector(100.f, 18.f, 240.f), Cyan, true);
					Post->ComponentTags.Add(TEXT("RunnerHazard"));
				}
			}
			else Position.Z = 120.f;
			UStaticMeshComponent* Hazard = AddBlock(Position, Size, Material, true);
			Hazard->ComponentTags.Add(TEXT("RunnerHazard"));
		}
	}
	const int32 CoinLane = SegmentIndex == 0 ? 0 : SafeLane;
	for (float X = 950.f; X <= 2150.f; X += 300.f)
		AddCoin(FVector(X, RunnerRules::LaneY(CoinLane), 95.f), Gold);
}
