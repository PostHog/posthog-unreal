#include "RunnerHUD.h"
#include "RunnerGameMode.h"
#include "RunnerGameInstance.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

void ARunnerHUD::DrawHUD()
{
	Super::DrawHUD();
	const ARunnerGameMode* Run = GetWorld()->GetAuthGameMode<ARunnerGameMode>();
	if (!Canvas || !Run) return;
	const float W = Canvas->SizeX;
	const float H = Canvas->SizeY;
	const float Scale = FMath::Clamp(W / 1280.f, 0.65f, 2.f);
	const FLinearColor Cyan(0.15f, 0.9f, 1.f);
	const FLinearColor Gold(1.f, 0.75f, 0.1f);
	const auto Text = [this, Scale](const FString& Value, FLinearColor Color, float X, float Y, float Size = 1.f)
	{
		DrawText(Value, Color, X, Y, GEngine->GetMediumFont(), Scale * Size);
	};
	const auto Center = [this, W, Scale](const FString& Value, FLinearColor Color, float Y, float Size)
	{
		float TW = 0.f, TH = 0.f;
		GetTextSize(Value, TW, TH, GEngine->GetMediumFont(), Scale * Size);
		DrawText(Value, Color, (W - TW) / 2.f, Y, GEngine->GetMediumFont(), Scale * Size);
	};

	DrawRect(FLinearColor(0.015f, 0.025f, 0.045f, 0.88f), 0.f, 0.f, W, 100.f * Scale);
	DrawRect(Cyan, 0.f, 100.f * Scale, W, 2.f);
	if (const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
		Text(Instance->AnalyticsStatus(), Cyan, W * 0.77f, 22.f * Scale, 0.6f);
	Text(Run->bHardMode ? TEXT("HARD MODE") : TEXT("NORMAL MODE"), Gold, W * 0.77f, 56.f * Scale, 0.7f);
	Text(TEXT("NEON RUNNER"), Cyan, 26.f * Scale, 16.f * Scale, 1.3f);
	Text(FString::Printf(TEXT("%d m"), FMath::FloorToInt(Run->DistanceMeters)),
		FLinearColor::White, 26.f * Scale, 53.f * Scale, 1.2f);
	Text(FString::Printf(TEXT("COINS  %d"), Run->Coins), Gold, W * 0.55f, 22.f * Scale);
	Text(FString::Printf(TEXT("SPEED  %.0f km/h"), Run->GetRunSpeed() * 0.036f),
		FLinearColor::White, W * 0.55f, 56.f * Scale);
	DrawRect(FLinearColor(0.015f, 0.025f, 0.045f, 0.8f), 0.f, H - 55.f * Scale, W, 55.f * Scale);
	Center(TEXT("A / D  LANES     SPACE  JUMP     S / DOWN  SLIDE"), FLinearColor::White, H - 37.f * Scale, 0.8f);

	if (Run->bGameOver)
	{
		DrawRect(FLinearColor(0.01f, 0.02f, 0.04f, 0.82f), 0.f, H * 0.28f, W, H * 0.4f);
		Center(TEXT("RUN OVER"), Cyan, H * 0.33f, 2.2f);
		Center(FString::Printf(TEXT("%d METERS  /  %d COINS"), FMath::FloorToInt(Run->DistanceMeters), Run->Coins),
			Gold, H * 0.46f, 1.1f);
		Center(TEXT("R / ENTER / SPACE TO RUN AGAIN"), FLinearColor::White, H * 0.57f, 0.9f);
	}
	else if (Run->Countdown > 0.f)
	{
		Center(TEXT("GET READY"), Cyan, H * 0.32f, 1.5f);
		Center(FString::FromInt(FMath::CeilToInt(Run->Countdown)), Gold, H * 0.43f, 3.f);
	}
}
