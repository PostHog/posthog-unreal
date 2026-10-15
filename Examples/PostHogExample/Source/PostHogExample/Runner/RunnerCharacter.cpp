#include "RunnerCharacter.h"
#include "RunnerGameMode.h"
#include "RunnerRules.h"
#include "Components/SceneComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "InputCoreTypes.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

ARunnerCharacter::ARunnerCharacter()
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationYaw = false;
	UCharacterMovementComponent* Movement = GetCharacterMovement();
	Movement->bOrientRotationToMovement = false;
	Movement->JumpZVelocity = 720.f;
	Movement->GravityScale = 2.f;
	Movement->AirControl = 1.f;
	Movement->MaxAcceleration = 12000.f;
	Movement->GetNavAgentPropertiesRef().bCanCrouch = true;
	Movement->SetCrouchedHalfHeight(48.f);
	Movement->MaxWalkSpeedCrouched = 1800.f;
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.f);

	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 650.f;
	CameraBoom->bUsePawnControlRotation = false;
	CameraBoom->bDoCollisionTest = false;
	CameraBoom->SetRelativeRotation(FRotator(-12.f, 0.f, 0.f));
	CameraBoom->SocketOffset = FVector(0.f, 0.f, 150.f);
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	FollowCamera->SetFieldOfView(85.f);

	Body = CreateDefaultSubobject<USceneComponent>(TEXT("Body"));
	Body->SetupAttachment(GetMesh());
	GetMesh()->SetRelativeLocation(FVector(0.f, 0.f, -96.f));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere"));
	const auto Part = [this](const TCHAR* Name, USceneComponent* Parent, UStaticMesh* Shape, FVector Position, FVector Size)
	{
		UStaticMeshComponent* Mesh = CreateDefaultSubobject<UStaticMeshComponent>(Name);
		Mesh->SetupAttachment(Parent);
		Mesh->SetStaticMesh(Shape);
		Mesh->SetRelativeLocation(Position);
		Mesh->SetRelativeScale3D(Size / 100.f);
		Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		return Mesh;
	};
	Part(TEXT("Torso"), Body, Cube.Object, FVector(0, 0, 116), FVector(32, 48, 56));
	Part(TEXT("Head"), Body, Sphere.Object, FVector(0, 0, 162), FVector(34, 34, 34));
	Part(TEXT("Visor"), Body, Cube.Object, FVector(17, 0, 165), FVector(6, 28, 12));
	for (int32 Index = 0; Index < 4; ++Index)
	{
		const bool bArm = Index < 2;
		const float Side = Index % 2 == 0 ? -1.f : 1.f;
		USceneComponent* Pivot = CreateDefaultSubobject<USceneComponent>(*FString::Printf(TEXT("Joint%d"), Index));
		Pivot->SetupAttachment(Body);
		Pivot->SetRelativeLocation(FVector(0, Side * (bArm ? 33.f : 13.f), bArm ? 138.f : 87.f));
		Part(*FString::Printf(TEXT("Limb%d"), Index), Pivot, Cube.Object,
			FVector(0, 0, bArm ? -28.f : -40.f), FVector(17, 17, bArm ? 56.f : 80.f));
		Limbs.Add(Pivot);
	}
	GetCapsuleComponent()->OnComponentHit.AddDynamic(this, &ARunnerCharacter::OnCapsuleHit);
	GetCapsuleComponent()->OnComponentBeginOverlap.AddDynamic(this, &ARunnerCharacter::OnCapsuleOverlap);
}

void ARunnerCharacter::BeginPlay()
{
	Super::BeginPlay();
	RunnerGameMode = GetWorld()->GetAuthGameMode<ARunnerGameMode>();
	// A Blueprint may supply its own skeletal mesh/animation instead of the built-in robot.
	Body->SetVisibility(GetMesh()->GetSkeletalMeshAsset() == nullptr, true);
	UMaterialInstanceDynamic* Material = UMaterialInstanceDynamic::Create(
		LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial")), this);
	Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.45f, 0.04f));
	TArray<USceneComponent*> Parts;
	Body->GetChildrenComponents(true, Parts);
	for (USceneComponent* Part : Parts)
		if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Part)) Mesh->SetMaterial(0, Material);
	SetActorRotation(FRotator::ZeroRotator);
	if (APlayerController* PC = Cast<APlayerController>(GetController()))
	{
		PC->SetControlRotation(FRotator::ZeroRotator);
		PC->SetInputMode(FInputModeGameOnly());
		PC->bShowMouseCursor = false;
	}
}

void ARunnerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (bSliding && !bIsCrouched && !GetCharacterMovement()->bWantsToCrouch)
	{
		bSliding = false;
		Body->SetRelativeScale3D(FVector::OneVector);
	}
	AnimateBody(DeltaSeconds);
	if (!RunnerGameMode || !RunnerGameMode->IsRunning()) return;

	GetCharacterMovement()->MaxWalkSpeed = RunnerGameMode->GetRunSpeed();
	GetCharacterMovement()->MaxWalkSpeedCrouched = RunnerGameMode->GetRunSpeed();
	AddMovementInput(FVector::ForwardVector);

	const float DesiredY = FMath::FInterpConstantTo(GetActorLocation().Y,
		RunnerRules::LaneY(TargetLane), DeltaSeconds, 1500.f);
	FHitResult Hit;
	AddActorWorldOffset(FVector(0.f, DesiredY - GetActorLocation().Y, 0.f), true, &Hit);
	if (Hit.bBlockingHit && Hit.GetComponent() && Hit.GetComponent()->ComponentHasTag(TEXT("RunnerHazard")))
	{
		RunnerGameMode->EndRun();
	}
	if (GetActorLocation().Z < -300.f) RunnerGameMode->EndRun();
}

void ARunnerCharacter::AnimateBody(float DeltaSeconds)
{
	const float Speed = GetVelocity().Size2D();
	Stride += DeltaSeconds * Speed / 70.f;
	for (int32 Index = 0; Index < Limbs.Num(); ++Index)
	{
		const float Sign = (Index == 0 || Index == 3) ? 1.f : -1.f;
		const float Swing = bSliding ? 0.f : FMath::Sin(Stride) * 35.f * FMath::Min(Speed / 500.f, 1.f);
		Limbs[Index]->SetRelativeRotation(FRotator(Sign * Swing, 0.f, 0.f));
	}
}

void ARunnerCharacter::SetupPlayerInputComponent(UInputComponent* Input)
{
	// These discrete controls do not install the template's free-movement mapping contexts.
	Input->BindKey(EKeys::A, IE_Pressed, this, &ARunnerCharacter::MoveLeft);
	Input->BindKey(EKeys::Left, IE_Pressed, this, &ARunnerCharacter::MoveLeft);
	Input->BindKey(EKeys::D, IE_Pressed, this, &ARunnerCharacter::MoveRight);
	Input->BindKey(EKeys::Right, IE_Pressed, this, &ARunnerCharacter::MoveRight);
	Input->BindKey(EKeys::SpaceBar, IE_Pressed, this, &ARunnerCharacter::JumpOrRestart);
	Input->BindKey(EKeys::Up, IE_Pressed, this, &ARunnerCharacter::JumpOrRestart);
	Input->BindKey(EKeys::S, IE_Pressed, this, &ARunnerCharacter::Slide);
	Input->BindKey(EKeys::Down, IE_Pressed, this, &ARunnerCharacter::Slide);
	Input->BindKey(EKeys::R, IE_Pressed, this, &ARunnerCharacter::RetryRun);
	Input->BindKey(EKeys::Enter, IE_Pressed, this, &ARunnerCharacter::RetryRun);
}

void ARunnerCharacter::MoveLeft()
{
	if (RunnerGameMode && RunnerGameMode->IsRunning()) TargetLane = RunnerRules::ClampLane(TargetLane - 1);
}

void ARunnerCharacter::MoveRight()
{
	if (RunnerGameMode && RunnerGameMode->IsRunning()) TargetLane = RunnerRules::ClampLane(TargetLane + 1);
}

void ARunnerCharacter::JumpOrRestart()
{
	if (!RunnerGameMode) return;
	if (RunnerGameMode->bGameOver) { RetryRun(); return; }
	if (RunnerGameMode->IsRunning() && !bSliding) Jump();
}

void ARunnerCharacter::Slide()
{
	if (!RunnerGameMode || !RunnerGameMode->IsRunning() || bSliding || !GetCharacterMovement()->IsMovingOnGround()) return;
	bSliding = true;
	Crouch();
	Body->SetRelativeScale3D(FVector(1.f, 1.f, 0.5f));
	GetWorldTimerManager().SetTimer(SlideTimer, this, &ARunnerCharacter::StopSlide, 0.8f, false);
}

void ARunnerCharacter::StopSlide()
{
	// Keep the slide silhouette until CharacterMovement has enough clearance to stand.
	UnCrouch();
}

void ARunnerCharacter::RetryRun()
{
	if (RunnerGameMode && RunnerGameMode->bGameOver) RunnerGameMode->RestartRun();
}

void ARunnerCharacter::OnCapsuleHit(UPrimitiveComponent*, AActor*, UPrimitiveComponent* OtherComponent,
	FVector, const FHitResult&)
{
	if (RunnerGameMode && OtherComponent && OtherComponent->ComponentHasTag(TEXT("RunnerHazard")))
		RunnerGameMode->EndRun();
}

void ARunnerCharacter::OnCapsuleOverlap(UPrimitiveComponent*, AActor*, UPrimitiveComponent* OtherComponent,
	int32, bool, const FHitResult&)
{
	if (!RunnerGameMode || !RunnerGameMode->IsRunning() || !OtherComponent ||
		!OtherComponent->ComponentHasTag(TEXT("RunnerCoin"))) return;

	OtherComponent->ComponentTags.Remove(TEXT("RunnerCoin"));
	OtherComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OtherComponent->SetVisibility(false, true);
	RunnerGameMode->CollectCoin();
}
