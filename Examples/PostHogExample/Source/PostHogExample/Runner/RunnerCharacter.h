#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "RunnerCharacter.generated.h"

class ARunnerGameMode;
class UPrimitiveComponent;
class USpringArmComponent;
class UCameraComponent;
class USceneComponent;
struct FHitResult;

/** Asset-light runner with an articulated built-in-shape character. */
UCLASS()
class ARunnerCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ARunnerCharacter();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;

	UFUNCTION(BlueprintCallable, Category="Runner")
	void MoveLeft();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void MoveRight();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void JumpOrRestart();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void Slide();
	UFUNCTION(BlueprintCallable, Category="Runner")
	void RetryRun();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	int32 TargetLane = 0;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Runner")
	bool bSliding = false;

private:
	UPROPERTY()
	TObjectPtr<ARunnerGameMode> RunnerGameMode;

	UPROPERTY()
	TObjectPtr<USpringArmComponent> CameraBoom;
	UPROPERTY()
	TObjectPtr<UCameraComponent> FollowCamera;
	UPROPERTY()
	TObjectPtr<USceneComponent> Body;
	UPROPERTY()
	TArray<TObjectPtr<USceneComponent>> Limbs;

	FTimerHandle SlideTimer;
	float Stride = 0.f;
	void AnimateBody(float DeltaSeconds);
	void StopSlide();

	UFUNCTION()
	void OnCapsuleHit(UPrimitiveComponent* HitComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, FVector NormalImpulse, const FHitResult& Hit);

	UFUNCTION()
	void OnCapsuleOverlap(UPrimitiveComponent* OverlappedComponent, AActor* OtherActor,
		UPrimitiveComponent* OtherComponent, int32 OtherBodyIndex, bool bFromSweep, const FHitResult& SweepResult);
};
