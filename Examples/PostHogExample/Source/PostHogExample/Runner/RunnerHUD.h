#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "RunnerHUD.generated.h"

class ARunnerGameMode;
class UTexture2D;
struct FSlateFontInfo;
enum class ERunnerMenu : uint8;
enum class ERunnerMenuAction : uint8;

/** Asset-free, Canvas-drawn menus and run HUD in a carved-stone style. */
UCLASS()
class ARunnerHUD : public AHUD
{
	GENERATED_BODY()

public:
	virtual void BeginPlay() override;
	virtual void DrawHUD() override;

private:
	using FKeyHints = TArray<TPair<FString, FString>>;

	UPROPERTY()
	TObjectPtr<UTexture2D> StoneTexture;
	float Scale = 1.f;

	void DrawTitle();
	void DrawMainMenu();
	void DrawRunBar(const ARunnerGameMode& Run);
	void DrawCountdown(const ARunnerGameMode& Run);
	void DrawPause();
	void DrawGameOver(const ARunnerGameMode& Run);
	void DrawSdkPanel(float Right, float Top);
	void DrawMenuList(ERunnerMenu Menu, float X, float Y, float Width);
	void DrawHintStrip(const FKeyHints& Hints);
	void DrawKeyHints(const FKeyHints& Hints, float CenterX, float Y, float Points);
	void DrawVignette(float Strength);

	void DrawStonePanel(float X, float Y, float Width, float Height, float Inset, float SlabOpacity = 0.9f);
	void DrawTablet(float X, float Y, float Width, float Height);
	void DrawInlay(float X, float Y, float Width, float Height);
	void DrawTorchBars(float LeftCenterX, float RightCenterX, float CenterY, float Height);
	void DrawRelicIcon(float CenterX, float CenterY, float Size);
	/** Text sizes are in points at a 1280-pixel-wide canvas. */
	FSlateFontInfo Font(float Points, bool bHeavy) const;
	void DrawShadowedText(const FString& Text, FLinearColor Color, float X, float Y, float Points, bool bHeavy = false);
	void DrawCenteredText(const FString& Text, FLinearColor Color, float CenterX, float Y, float Points, bool bHeavy = false);
	FVector2D MeasureText(const FString& Text, float Points, bool bHeavy = false) const;
	FString ItemLabel(ERunnerMenuAction Action) const;
	bool IsItemEnabled(ERunnerMenuAction Action) const;
};
