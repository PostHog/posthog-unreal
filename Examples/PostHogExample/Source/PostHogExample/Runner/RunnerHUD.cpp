#include "RunnerHUD.h"
#include "RunnerGameInstance.h"
#include "RunnerGameMode.h"
#include "RunnerMenu.h"
#include "RunnerPlayerController.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"

namespace
{
	// Palette authored in sRGB; Canvas colors are linear.
	const FLinearColor Sandstone(FColor(140, 115, 77));
	const FLinearColor SandstoneLight(FColor(214, 190, 140));
	const FLinearColor SandstoneShadow(FColor(70, 52, 30));
	const FLinearColor DarkStone(FColor(26, 20, 15));
	const FLinearColor Gold(FColor(255, 184, 51));
	const FLinearColor Torch(FColor(255, 128, 26));
	const FLinearColor Parchment(FColor(242, 224, 178));
	const FLinearColor Moss(FColor(64, 89, 38));
	const FLinearColor MossLight(FColor(150, 196, 96));
	const FLinearColor Muted(FColor(128, 118, 100));
	const FLinearColor TextShadow(0.f, 0.f, 0.f, 0.85f);
	constexpr int32 StoneSize = 128;
	constexpr float RowHeight = 54.f;
	constexpr float BarHeight = 84.f;
	constexpr float StripHeight = 46.f;

	/** Exposes the Slate-backed measurement Canvas already uses to lay out runtime-font text. */
	struct FMeasuredText : FCanvasTextItem
	{
		using FCanvasTextItem::FCanvasTextItem;
		FVector2D Size() const { return GetTextSize(1.f); }
	};

	FLinearColor WithAlpha(FLinearColor Color, float Alpha)
	{
		Color.A *= Alpha;
		return Color;
	}

	float Hash(int32 X, int32 Y, int32 Period)
	{
		X = (X % Period + Period) % Period;
		Y = (Y % Period + Period) % Period;
		uint32 H = uint32(X) * 374761393u + uint32(Y) * 668265263u;
		H = (H ^ (H >> 13)) * 1274126177u;
		return float((H ^ (H >> 16)) & 0xffff) / 65535.f;
	}

	/** Tileable value noise sampled on a lattice that repeats every Period cells. */
	float ValueNoise(float X, float Y, int32 Period)
	{
		const int32 X0 = FMath::FloorToInt(X), Y0 = FMath::FloorToInt(Y);
		const float FX = FMath::SmoothStep(0.f, 1.f, X - X0), FY = FMath::SmoothStep(0.f, 1.f, Y - Y0);
		const float Top = FMath::Lerp(Hash(X0, Y0, Period), Hash(X0 + 1, Y0, Period), FX);
		const float Bottom = FMath::Lerp(Hash(X0, Y0 + 1, Period), Hash(X0 + 1, Y0 + 1, Period), FX);
		return FMath::Lerp(Top, Bottom, FY);
	}

	UTexture2D* MakeStoneTexture()
	{
		TArray<uint8> Pixels;
		Pixels.SetNumUninitialized(StoneSize * StoneSize * 4);
		for (int32 Y = 0; Y < StoneSize; ++Y)
		{
			for (int32 X = 0; X < StoneSize; ++X)
			{
				float Value = 0.f, Amplitude = 0.5f;
				for (int32 Octave = 0, Period = 4; Octave < 5; ++Octave, Period *= 2, Amplitude *= 0.5f)
					Value += Amplitude * ValueNoise(X * Period / float(StoneSize), Y * Period / float(StoneSize), Period);
				// Bright weathered surface with occasional dark pits.
				const float Pit = FMath::Clamp((0.32f - Value) * 4.f, 0.f, 1.f);
				const uint8 Shade = uint8(FMath::Clamp(0.62f + Value * 0.5f - Pit * 0.35f, 0.f, 1.f) * 255.f);
				uint8* Pixel = &Pixels[(Y * StoneSize + X) * 4];
				Pixel[0] = Pixel[1] = Pixel[2] = Shade;
				Pixel[3] = 255;
			}
		}
		UTexture2D* Texture = UTexture2D::CreateTransient(StoneSize, StoneSize, PF_B8G8R8A8, NAME_None, Pixels);
		if (!Texture) return nullptr;
		Texture->AddressX = TA_Wrap;
		Texture->AddressY = TA_Wrap;
		Texture->UpdateResource();
		return Texture;
	}
}

void ARunnerHUD::BeginPlay()
{
	Super::BeginPlay();
	StoneTexture = MakeStoneTexture();
}

void ARunnerHUD::DrawHUD()
{
	Super::DrawHUD();
	const ARunnerGameMode* Run = GetWorld()->GetAuthGameMode<ARunnerGameMode>();
	if (!Canvas || !Run || !GEngine || !GEngine->GetMediumFont()) return;
	Scale = FMath::Clamp(Canvas->SizeX / 1280.f, 0.65f, 2.f);

	if (Run->Menu == ERunnerMenu::Title) return DrawTitle();
	if (Run->Menu == ERunnerMenu::Main) return DrawMainMenu();
	DrawRunBar(*Run);
	if (Run->bGameOver) DrawGameOver(*Run);
	else if (Run->Menu == ERunnerMenu::Pause) DrawPause();
	else
	{
		if (Run->Countdown > 0.f) DrawCountdown(*Run);
		DrawHintStrip({{TEXT("A / D"), TEXT("LANES")}, {TEXT("SPACE"), TEXT("JUMP")},
			{TEXT("S / DOWN"), TEXT("SLIDE")}, {TEXT("ESC"), TEXT("PAUSE")}});
	}
}

void ARunnerHUD::DrawTitle()
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale;
	DrawVignette(0.75f);
	const FString Wordmark = TEXT("TIM RAIDER");
	const FVector2D Mark = MeasureText(Wordmark, 96.f, true);
	const float MarkY = H * 0.13f;
	const FString Tagline = TEXT("ESCAPE THE JUNGLE TEMPLE");
	const FVector2D Tag = MeasureText(Tagline, 20.f);
	const float TagY = MarkY + Mark.Y + 4.f * S;
	const float TabletW = Mark.X + 150.f * S, TabletY = MarkY - 26.f * S;
	DrawTablet((W - TabletW) / 2.f, TabletY, TabletW, TagY + Tag.Y + 26.f * S - TabletY);
	// A dark outline and a deep drop shadow read as letters carved out of the temple wall.
	FSlateFontInfo MarkFont = Font(96.f, true);
	MarkFont.OutlineSettings.OutlineSize = FMath::RoundToInt(3.f * S);
	MarkFont.OutlineSettings.OutlineColor = FLinearColor(0.02f, 0.012f, 0.004f);
	FCanvasTextItem MarkItem(FVector2D((W - Mark.X) / 2.f, MarkY), FText::FromString(Wordmark), MarkFont, Gold);
	MarkItem.EnableShadow(FLinearColor(0.f, 0.f, 0.f, 0.8f), FVector2D(6.f, 7.f) * S);
	Canvas->DrawItem(MarkItem);

	DrawCenteredText(Tagline, Parchment, W / 2.f, TagY, 20.f);
	const float RuleY = TagY + Tag.Y / 2.f;
	for (const float Side : {-1.f, 1.f})
	{
		const float Inner = W / 2.f + Side * (Tag.X / 2.f + 22.f * S);
		const float Outer = Inner + Side * 110.f * S;
		DrawRect(TextShadow, FMath::Min(Inner, Outer) + 2.f * S, RuleY + 1.f * S, FMath::Abs(Outer - Inner), 2.f * S);
		DrawRect(Gold, FMath::Min(Inner, Outer), RuleY - 1.f * S, FMath::Abs(Outer - Inner), 2.f * S);
		DrawRect(Gold, Outer - 6.f * S, RuleY - 6.f * S, 12.f * S, 12.f * S);
		DrawRect(DarkStone, Outer - 3.f * S, RuleY - 3.f * S, 6.f * S, 6.f * S);
	}

	const float Pulse = 0.5f + 0.5f * FMath::Sin(GetWorld()->GetRealTimeSeconds() * 3.5f);
	const FString Prompt = TEXT("PRESS ENTER");
	const FVector2D PromptSize = MeasureText(Prompt, 26.f);
	const float PromptY = H * 0.66f;
	DrawStonePanel(W / 2.f - PromptSize.X / 2.f - 40.f * S, PromptY - 16.f * S, PromptSize.X + 80.f * S,
		PromptSize.Y + 32.f * S, 7.f * S);
	DrawCenteredText(Prompt, FMath::Lerp(Muted, Gold, Pulse), W / 2.f, PromptY, 26.f);

	if (const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
		DrawCenteredText(TEXT("A POSTHOG UNREAL SDK DEMO   |   ") + Instance->AnalyticsStatus(),
			WithAlpha(Parchment, 0.85f), W / 2.f, H - 48.f * S, 13.f);
}

void ARunnerHUD::DrawMainMenu()
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale;
	DrawVignette(0.45f);
	const float PanelX = 64.f * S, PanelW = 420.f * S;
	const float PanelH = 124.f * S + RunnerMenu::Items(ERunnerMenu::Main).Num() * RowHeight * S;
	const float PanelY = FMath::Max(40.f * S, (H - StripHeight * S - PanelH) * 0.4f);
	DrawStonePanel(PanelX, PanelY, PanelW, PanelH, 10.f * S);
	DrawInlay(PanelX + 22.f * S, PanelY + 18.f * S, PanelW - 44.f * S, 62.f * S);
	DrawTorchBars(PanelX + 34.f * S, PanelX + PanelW - 34.f * S, PanelY + 49.f * S, 36.f * S);
	DrawCenteredText(TEXT("TIM RAIDER"), Gold, PanelX + PanelW / 2.f, PanelY + 24.f * S, 36.f, true);
	DrawRect(WithAlpha(Gold, 0.7f), PanelX + 40.f * S, PanelY + 90.f * S, PanelW - 80.f * S, 2.f * S);
	DrawMenuList(ERunnerMenu::Main, PanelX + 10.f * S, PanelY + 104.f * S, PanelW - 20.f * S);
	DrawSdkPanel(W - 64.f * S, PanelY);
	DrawHintStrip({{TEXT("UP / DOWN"), TEXT("SELECT")}, {TEXT("ENTER"), TEXT("CONFIRM")}, {TEXT("ESC"), TEXT("BACK")}});
}

void ARunnerHUD::DrawSdkPanel(float Right, float Top)
{
	const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance());
	if (!Instance) return;
	const float S = Scale, PanelW = 400.f * S, LineH = 27.f * S;
	TArray<TPair<FString, FLinearColor>> Lines;
	Lines.Add({Instance->AnalyticsStatus(), Instance->IsAnalyticsEnabled() ? MossLight : Muted});
	Lines.Add({TEXT("DIFFICULTY FLAG: ") + Instance->DifficultyFlagLabel(), Parchment});
	if (Instance->HasClient())
	{
		Lines.Add({TEXT("DISTINCT ID: ") + Instance->ShortDistinctId(), Parchment});
		Lines.Add({TEXT("SESSION ID: ") + Instance->ShortSessionId(), Parchment});
	}
	else
	{
		Lines.Add({TEXT("SET POSTHOG_TOKEN TO CAPTURE"), Muted});
	}
	const FString Reload = Instance->FlagReloadStatus();
	if (!Reload.IsEmpty()) Lines.Add({Reload, Torch});

	const float PanelH = 70.f * S + Lines.Num() * LineH;
	const float X = Right - PanelW, Y = Top;
	DrawStonePanel(X, Y, PanelW, PanelH, 8.f * S);
	DrawShadowedText(TEXT("POSTHOG SDK"), Gold, X + 28.f * S, Y + 20.f * S, 17.f, true);
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
		DrawShadowedText(Lines[Index].Key, Lines[Index].Value, X + 28.f * S, Y + 54.f * S + Index * LineH, 14.f);
}

void ARunnerHUD::DrawMenuList(ERunnerMenu Menu, float X, float Y, float Width)
{
	const float S = Scale, RowH = RowHeight * S;
	const ARunnerPlayerController* Controller = Cast<ARunnerPlayerController>(GetOwningPlayerController());
	const int32 Selected = Controller ? Controller->GetMenuIndex() : 0;
	const TConstArrayView<ERunnerMenuAction> Items = RunnerMenu::Items(Menu);
	for (int32 Index = 0; Index < Items.Num(); ++Index)
	{
		const float RowY = Y + Index * RowH;
		const bool bSelected = Index == Selected;
		const bool bEnabled = IsItemEnabled(Items[Index]);
		const FString Label = ItemLabel(Items[Index]);
		const FVector2D Size = MeasureText(Label, 22.f);
		const float TextY = RowY + (RowH - Size.Y) / 2.f;
		if (bSelected)
		{
			DrawRect(WithAlpha(Torch, 0.25f), X + 14.f * S, RowY + 5.f * S, Width - 28.f * S, RowH - 10.f * S);
			DrawRect(Gold, X + 14.f * S, RowY + 5.f * S, 4.f * S, RowH - 10.f * S);
			DrawRect(Gold, X + Width - 18.f * S, RowY + 5.f * S, 4.f * S, RowH - 10.f * S);
			const float Mid = X + Width / 2.f;
			const float Arrow = MeasureText(TEXT(">"), 22.f).X;
			DrawShadowedText(TEXT(">"), Gold, Mid - Size.X / 2.f - Arrow - 16.f * S, TextY, 22.f, true);
			DrawShadowedText(TEXT("<"), Gold, Mid + Size.X / 2.f + 16.f * S, TextY, 22.f, true);
		}
		const FLinearColor Color = !bEnabled ? Muted : bSelected ? Gold : Parchment;
		DrawCenteredText(Label, Color, X + Width / 2.f, TextY, 22.f);
	}
}

void ARunnerHUD::DrawRunBar(const ARunnerGameMode& Run)
{
	const float W = Canvas->SizeX, S = Scale;
	DrawStonePanel(-8.f * S, -8.f * S, W + 16.f * S, BarHeight * S + 8.f * S, 7.f * S);
	const int32 Meters = FMath::FloorToInt(Run.DistanceMeters);
	const auto Cell = [this, S](const FString& Label, const FString& Value, FLinearColor Color, float X)
	{
		DrawShadowedText(Label, Gold, X, 12.f * S, 11.f, true);
		DrawShadowedText(Value, Color, X, 30.f * S, 24.f, true);
	};
	Cell(TEXT("DISTANCE"), FString::Printf(TEXT("%d m"), Meters), Parchment, 36.f * S);
	DrawShadowedText(TEXT("RELICS"), Gold, W * 0.2f, 12.f * S, 11.f, true);
	DrawRelicIcon(W * 0.2f + 12.f * S, 47.f * S, 26.f * S);
	DrawShadowedText(FString::FromInt(Run.Coins), Gold, W * 0.2f + 32.f * S, 30.f * S, 24.f, true);
	Cell(TEXT("SCORE"), FString::FromInt(URunnerGameInstance::ScoreFor(Meters, Run.Coins)), Parchment, W * 0.33f);
	Cell(TEXT("SPEED"), FString::Printf(TEXT("%.0f km/h"), Run.GetRunSpeed() * 0.036f), Parchment, W * 0.47f);

	const FString Mode = Run.bHardMode ? TEXT("HARD MODE") : TEXT("NORMAL MODE");
	DrawShadowedText(Mode, Run.bHardMode ? Torch : MossLight, W - 36.f * S - MeasureText(Mode, 18.f, true).X,
		14.f * S, 18.f, true);
	if (const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
	{
		const FString Status = Instance->AnalyticsStatus();
		DrawShadowedText(Status, Parchment, W - 36.f * S - MeasureText(Status, 12.f).X, 48.f * S, 12.f);
	}
}

void ARunnerHUD::DrawCountdown(const ARunnerGameMode& Run)
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale;
	const float PanelW = 240.f * S, PanelH = 180.f * S, X = (W - PanelW) / 2.f, Y = H * 0.28f;
	DrawStonePanel(X, Y, PanelW, PanelH, 10.f * S);
	DrawCenteredText(TEXT("GET READY"), Gold, W / 2.f, Y + 22.f * S, 18.f, true);
	DrawCenteredText(FString::FromInt(FMath::CeilToInt(Run.Countdown)), Parchment, W / 2.f, Y + 52.f * S, 72.f, true);
}

void ARunnerHUD::DrawPause()
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), 0.f, BarHeight * S, W, H - BarHeight * S);
	const float PanelW = 440.f * S;
	const float PanelH = 124.f * S + RunnerMenu::Items(ERunnerMenu::Pause).Num() * RowHeight * S;
	const float X = (W - PanelW) / 2.f;
	const float Y = BarHeight * S + FMath::Max(16.f * S, (H - (BarHeight + StripHeight) * S - PanelH) / 2.f);
	DrawStonePanel(X, Y, PanelW, PanelH, 10.f * S);
	DrawCenteredText(TEXT("PAUSED"), Gold, W / 2.f, Y + 24.f * S, 36.f, true);
	DrawRect(WithAlpha(Gold, 0.7f), X + 40.f * S, Y + 90.f * S, PanelW - 80.f * S, 2.f * S);
	DrawMenuList(ERunnerMenu::Pause, X + 10.f * S, Y + 104.f * S, PanelW - 20.f * S);
	DrawHintStrip({{TEXT("UP / DOWN"), TEXT("SELECT")}, {TEXT("ENTER"), TEXT("CONFIRM")}, {TEXT("ESC"), TEXT("RESUME")}});
}

void ARunnerHUD::DrawGameOver(const ARunnerGameMode& Run)
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.45f), 0.f, BarHeight * S, W, H - BarHeight * S);
	const float PanelW = 580.f * S, PanelH = 376.f * S;
	const float X = (W - PanelW) / 2.f;
	const float Y = BarHeight * S + FMath::Max(16.f * S, (H - BarHeight * S - PanelH) / 2.f);
	DrawStonePanel(X, Y, PanelW, PanelH, 10.f * S);
	const int32 Meters = FMath::FloorToInt(Run.DistanceMeters);
	DrawCenteredText(TEXT("RUN OVER"), Gold, W / 2.f, Y + 22.f * S, 48.f, true);
	DrawRect(WithAlpha(Gold, 0.7f), X + 60.f * S, Y + 106.f * S, PanelW - 120.f * S, 2.f * S);
	DrawCenteredText(TEXT("SCORE"), Gold, W / 2.f, Y + 120.f * S, 14.f, true);
	DrawCenteredText(FString::FromInt(URunnerGameInstance::ScoreFor(Meters, Run.Coins)), Parchment, W / 2.f,
		Y + 138.f * S, 44.f, true);

	const FString Distance = FString::Printf(TEXT("%d m   |   "), Meters);
	const FString Relics = FString::Printf(TEXT("%d RELICS"), Run.Coins);
	const float Icon = 26.f * S, IconGap = 10.f * S;
	const FVector2D DistanceSize = MeasureText(Distance, 20.f);
	float LineX = W / 2.f - (DistanceSize.X + Icon + IconGap + MeasureText(Relics, 20.f).X) / 2.f;
	const float LineY = Y + 206.f * S;
	DrawShadowedText(Distance, Parchment, LineX, LineY, 20.f);
	LineX += DistanceSize.X;
	DrawRelicIcon(LineX + Icon / 2.f, LineY + DistanceSize.Y / 2.f, Icon);
	DrawShadowedText(Relics, Parchment, LineX + Icon + IconGap, LineY, 20.f);

	if (const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance()))
	{
		const FString Best = FString::Printf(TEXT("%d   |   %d m"), Instance->GetBestScore(), Instance->GetBestDistance());
		if (Instance->IsLastRunBest())
			DrawKeyHints({{TEXT("NEW BEST"), Best}}, W / 2.f, Y + 246.f * S, 18.f);
		else
			DrawKeyHints({{TEXT("BEST"), Best}}, W / 2.f, Y + 246.f * S, 18.f);
	}
	DrawRect(WithAlpha(Gold, 0.4f), X + 90.f * S, Y + 282.f * S, PanelW - 180.f * S, 1.f * S);
	DrawKeyHints({{TEXT("SPACE / ENTER / R"), TEXT("RUN AGAIN")}}, W / 2.f, Y + 294.f * S, 17.f);
	DrawKeyHints({{TEXT("ESC"), TEXT("MAIN MENU")}}, W / 2.f, Y + 328.f * S, 17.f);
}

void ARunnerHUD::DrawHintStrip(const FKeyHints& Hints)
{
	const float W = Canvas->SizeX, H = Canvas->SizeY, S = Scale, StripH = StripHeight * S;
	DrawStonePanel(-8.f * S, H - StripH, W + 16.f * S, StripH + 8.f * S, 6.f * S);
	DrawKeyHints(Hints, W / 2.f, H - StripH + (StripH - MeasureText(TEXT("A"), 15.f).Y) / 2.f + 2.f * S, 15.f);
}

void ARunnerHUD::DrawKeyHints(const FKeyHints& Hints, float CenterX, float Y, float Points)
{
	const float Gap = 10.f * Scale, Spacing = 44.f * Scale;
	float Total = -Spacing;
	for (const auto& Hint : Hints)
		Total += MeasureText(Hint.Key, Points, true).X + Gap + MeasureText(Hint.Value, Points).X + Spacing;
	float X = CenterX - Total / 2.f;
	for (const auto& Hint : Hints)
	{
		DrawShadowedText(Hint.Key, Gold, X, Y, Points, true);
		X += MeasureText(Hint.Key, Points, true).X + Gap;
		DrawShadowedText(Hint.Value, Parchment, X, Y, Points);
		X += MeasureText(Hint.Value, Points).X + Spacing;
	}
}

void ARunnerHUD::DrawVignette(float Strength)
{
	const float W = Canvas->SizeX, H = Canvas->SizeY;
	DrawRect(FLinearColor(0.f, 0.f, 0.f, Strength * 0.3f), 0.f, 0.f, W, H);
	constexpr int32 Bands = 8;
	const float BandH = H * 0.035f;
	for (int32 Band = 0; Band < Bands; ++Band)
	{
		const FLinearColor Shade(0.02f, 0.01f, 0.f, Strength * 0.1f * (Bands - Band) / Bands);
		DrawRect(Shade, 0.f, Band * BandH, W, BandH);
		DrawRect(Shade, 0.f, H - (Band + 1) * BandH, W, BandH);
	}
}

void ARunnerHUD::DrawStonePanel(float X, float Y, float Width, float Height, float Inset, float SlabOpacity)
{
	const float S = Scale, Edge = 3.f * S, Line = FMath::Max(1.f, 2.f * S);
	DrawRect(DarkStone, X - Edge, Y - Edge, Width + 2.f * Edge, Height + 2.f * Edge);
	if (StoneTexture)
	{
		const float Tile = StoneSize * 1.5f * S;
		DrawTexture(StoneTexture, X, Y, Width, Height, X / Tile, Y / Tile, Width / Tile, Height / Tile, Sandstone, BLEND_Opaque);
	}
	else
	{
		DrawRect(Sandstone, X, Y, Width, Height);
	}
	DrawRect(SandstoneLight, X, Y, Width, Line);
	DrawRect(SandstoneLight, X, Y, Line, Height);
	DrawRect(SandstoneShadow, X, Y + Height - Line, Width, Line);
	DrawRect(SandstoneShadow, X + Width - Line, Y, Line, Height);

	// The recessed slab is lit from above, so its bevel is the inverse of the frame's.
	const float IX = X + Inset, IY = Y + Inset, IW = Width - 2.f * Inset, IH = Height - 2.f * Inset;
	DrawRect(WithAlpha(DarkStone, SlabOpacity), IX, IY, IW, IH);
	DrawRect(SandstoneShadow, IX, IY, IW, Line);
	DrawRect(SandstoneShadow, IX, IY, Line, IH);
	DrawRect(WithAlpha(SandstoneLight, 0.6f), IX, IY + IH - Line, IW, Line);
	DrawRect(WithAlpha(SandstoneLight, 0.6f), IX + IW - Line, IY, Line, IH);
	DrawRect(WithAlpha(Moss, 0.5f), IX + Line, IY + Line, IW - 2.f * Line, Line);

	const float Arm = 14.f * S, Thick = 3.f * S;
	for (const bool bRight : {false, true})
	{
		for (const bool bBottom : {false, true})
		{
			const float CX = bRight ? X + Width + Edge : X - Edge;
			const float CY = bBottom ? Y + Height + Edge : Y - Edge;
			DrawRect(Gold, bRight ? CX - Arm : CX, bBottom ? CY - Thick : CY, Arm, Thick);
			DrawRect(Gold, bRight ? CX - Thick : CX, bBottom ? CY - Arm : CY, Thick, Arm);
		}
	}
}

void ARunnerHUD::DrawTablet(float X, float Y, float Width, float Height)
{
	const float S = Scale;
	DrawStonePanel(X, Y, Width, Height, 12.f * S, 0.62f);
	DrawInlay(X + 22.f * S, Y + 20.f * S, Width - 44.f * S, Height - 40.f * S);
	DrawTorchBars(X - 26.f * S, X + Width + 26.f * S, Y + Height / 2.f, Height * 0.6f);
}

void ARunnerHUD::DrawInlay(float X, float Y, float Width, float Height)
{
	const float Line = FMath::Max(1.f, 1.5f * Scale);
	const FLinearColor Inlay = WithAlpha(Gold, 0.75f);
	DrawRect(Inlay, X, Y, Width, Line);
	DrawRect(Inlay, X, Y + Height - Line, Width, Line);
	DrawRect(Inlay, X, Y, Line, Height);
	DrawRect(Inlay, X + Width - Line, Y, Line, Height);
}

void ARunnerHUD::DrawTorchBars(float LeftCenterX, float RightCenterX, float CenterY, float Height)
{
	const float S = Scale, BarW = 7.f * S;
	const float Flicker = 0.85f + 0.15f * FMath::Sin(GetWorld()->GetRealTimeSeconds() * 9.f);
	for (const float CenterX : {LeftCenterX, RightCenterX})
	{
		DrawRect(WithAlpha(Torch, 0.18f * Flicker), CenterX - BarW * 2.f, CenterY - Height * 0.6f, BarW * 4.f, Height * 1.2f);
		DrawRect(DarkStone, CenterX - BarW / 2.f - 2.f * S, CenterY - Height / 2.f - 2.f * S, BarW + 4.f * S, Height + 4.f * S);
		DrawRect(WithAlpha(Torch, Flicker), CenterX - BarW / 2.f, CenterY - Height / 2.f, BarW, Height);
		DrawRect(WithAlpha(Gold, Flicker), CenterX - BarW / 6.f, CenterY - Height / 2.f, BarW / 3.f, Height);
	}
}

void ARunnerHUD::DrawRelicIcon(float CenterX, float CenterY, float Size)
{
	const FVector2D Pivot(0.5f, 0.5f);
	const auto Diamond = [&](float Extent, FLinearColor Color)
	{
		const float Side = Extent / UE_SQRT_2;
		if (StoneTexture)
			DrawTexture(StoneTexture, CenterX - Side / 2.f, CenterY - Side / 2.f, Side, Side, 0.f, 0.f, 0.25f, 0.25f,
				Color, BLEND_Translucent, 1.f, false, 45.f, Pivot);
		else
			DrawRect(Color, CenterX - Side / 2.f, CenterY - Side / 2.f, Side, Side);
	};
	Diamond(Size + 4.f * Scale, DarkStone);
	Diamond(Size, Gold);
	Diamond(Size * 0.55f, DarkStone);
	Diamond(Size * 0.25f, Gold);
}

FSlateFontInfo ARunnerHUD::Font(float Points, bool bHeavy) const
{
	FSlateFontInfo Info = GEngine->GetMediumFont()->GetLegacySlateFontInfo();
	Info.TypefaceFontName = bHeavy ? TEXT("Bold") : TEXT("Regular");
	Info.Size = FMath::RoundToFloat(Points * Scale);
	return Info;
}

void ARunnerHUD::DrawShadowedText(const FString& Text, FLinearColor Color, float X, float Y, float Points, bool bHeavy)
{
	FCanvasTextItem Item(FVector2D(X, Y), FText::FromString(Text), Font(Points, bHeavy), Color);
	Item.EnableShadow(WithAlpha(TextShadow, Color.A), FVector2D(1.f, 1.f) * FMath::Clamp(Points * Scale / 12.f, 1.f, 3.f));
	Canvas->DrawItem(Item);
}

void ARunnerHUD::DrawCenteredText(const FString& Text, FLinearColor Color, float CenterX, float Y, float Points, bool bHeavy)
{
	DrawShadowedText(Text, Color, CenterX - MeasureText(Text, Points, bHeavy).X / 2.f, Y, Points, bHeavy);
}

FVector2D ARunnerHUD::MeasureText(const FString& Text, float Points, bool bHeavy) const
{
	return FMeasuredText(FVector2D::ZeroVector, FText::FromString(Text), Font(Points, bHeavy), FLinearColor::White).Size();
}

FString ARunnerHUD::ItemLabel(ERunnerMenuAction Action) const
{
	switch (Action)
	{
	case ERunnerMenuAction::StartRun: return TEXT("START RUN");
	case ERunnerMenuAction::Resume: return TEXT("RESUME");
	case ERunnerMenuAction::Restart: return TEXT("RESTART");
	case ERunnerMenuAction::ToggleAnalytics:
	{
		const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance());
		return Instance && Instance->IsAnalyticsEnabled() ? TEXT("ANALYTICS: ON") : TEXT("ANALYTICS: OFF");
	}
	case ERunnerMenuAction::ReloadFlags: return TEXT("RELOAD FLAGS");
	case ERunnerMenuAction::MainMenu: return TEXT("MAIN MENU");
	case ERunnerMenuAction::Quit: return TEXT("QUIT");
	}
	return FString();
}

bool ARunnerHUD::IsItemEnabled(ERunnerMenuAction Action) const
{
	if (Action != ERunnerMenuAction::ToggleAnalytics && Action != ERunnerMenuAction::ReloadFlags) return true;
	const URunnerGameInstance* Instance = Cast<URunnerGameInstance>(GetGameInstance());
	return Instance && Instance->HasClient();
}
