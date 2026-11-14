#include "PostHogSmokeGameInstance.h"
#include "PostHogSubsystem.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "Rendering/DrawElements.h"
#include "Styling/CoreStyle.h"
#include "UnrealClient.h"
#include "Widgets/Colors/SColorBlock.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SWindow.h"

namespace
{
class SReplayPattern final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SReplayPattern) {} SLATE_END_ARGS()
    void Construct(const FArguments&) {}
    mutable int32 Paints = 0;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(640, 480); }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect&,
        FSlateWindowElementList& Elements, int32 Layer, const FWidgetStyle&, bool) const override
    {
        ++Paints;
        const FVector2D Half = Geometry.GetLocalSize() / 2;
        const FLinearColor Colors[] = {FLinearColor::Red, FLinearColor::Green, FLinearColor::Blue, FLinearColor::White};
        for (int32 Index = 0; Index < 4; ++Index)
        {
            FSlateDrawElement::MakeBox(Elements, Layer,
                Geometry.ToPaintGeometry(Half, FSlateLayoutTransform(FVector2D(Index % 2 * Half.X, Index / 2 * Half.Y))),
                FCoreStyle::Get().GetBrush(TEXT("WhiteBrush")), ESlateDrawEffect::None, Colors[Index]);
        }
        return Layer;
    }
};
}
void UPostHogSmokeGameInstance::Init()
{
    Super::Init();
    if (!FParse::Param(FCommandLine::Get(), TEXT("PostHogReplaySmoke"))) { return; }
    const FString Host = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_HOST"));
    const FString Storage = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_STORAGE"));
    Controls = FPlatformMisc::GetEnvironmentVariable(TEXT("POSTHOG_TEST_CONTROL_DIRECTORY"));
    if (!Host.StartsWith(TEXT("http://127.0.0.1:")) || Storage.IsEmpty() || Controls.IsEmpty() || !FApp::CanEverRender() || GUsingNullRHI)
    { FPlatformMisc::RequestExitWithStatus(false, 2); return; }
    FPostHogConfig Config;
    Config.ProjectToken = TEXT("phc_MOCK"); Config.Host = Host; Config.StorageRoot = Storage; Config.ClientName = TEXT("replay-smoke");
    Config.bPreloadFeatureFlags = true; Config.FlushAt = 1; Config.FlushIntervalMilliseconds = 1000;
    Config.Replay.JpegQuality = 80; Config.Replay.IntervalMs = 100; Config.Replay.MaxDimension = 320;
    bManualStart = FParse::Param(FCommandLine::Get(), TEXT("PostHogReplayManualStart"));
    if (!GetSubsystem<UPostHogSubsystem>()->SetupWithConfig(Config, true)) { FPlatformMisc::RequestExitWithStatus(false, 3); return; }
    Deadline = FPlatformTime::Seconds() + 120;
    TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UPostHogSmokeGameInstance::TickSmoke));
}
bool UPostHogSmokeGameInstance::TickSmoke(float)
{
    if (FPlatformTime::Seconds() > Deadline) { FPlatformMisc::RequestExitWithStatus(false, 4); return false; }
    if (IFileManager::Get().FileExists(*(Controls / TEXT("stop")))) { FPlatformMisc::RequestExit(false); return false; }
    auto* Subsystem = GetSubsystem<UPostHogSubsystem>(); auto Client = Subsystem->GetClient(); auto* Viewport = GetGameViewportClient();
    if (!Client || !Viewport || !Viewport->Viewport) { return true; }
    if (!Pattern)
    {
        Pattern = SNew(SReplayPattern); Viewport->AddViewportWidgetContent(Pattern.ToSharedRef(), 100);
        OtherWindow = SNew(SWindow).Title(FText::FromString(TEXT("Unrelated magenta surface"))).ClientSize(FVector2D(240, 180)).FocusWhenFirstShown(false)
            [SNew(SColorBlock).Color(FLinearColor(1, 0, 1, 1))];
        FSlateApplication::Get().AddWindow(OtherWindow.ToSharedRef());
    }
    if (!bCaptured && StaticCastSharedPtr<SReplayPattern>(Pattern)->Paints > 0)
    {
        FPostHogProperties Properties; Properties.SetString(TEXT("capture_mode"), TEXT("automatic-viewport"));
        Properties.SetString(TEXT("rhi_adapter"), GRHIAdapterName); Properties.SetNumber(TEXT("rhi_vendor_id"), GRHIVendorId);
        const auto Size = Viewport->Viewport->GetSizeXY();
        Properties.SetNumber(TEXT("viewport_width"), Size.X); Properties.SetNumber(TEXT("viewport_height"), Size.Y);
        bCaptured = Client->Capture(TEXT("replay_smoke_ready"), Properties) == EPostHogResult::Accepted;
    }
    if (bCaptured && !bReplayStarted && (!bManualStart || IFileManager::Get().FileExists(*(Controls / TEXT("start")))))
    {
        for (int32 Attempt = 0; Attempt < 2; ++Attempt)
        {
            struct { EPostHogResult ReturnValue = EPostHogResult::Closed; } Params;
            Subsystem->ProcessEvent(Subsystem->FindFunctionChecked(TEXT("StartSessionReplay")), &Params);
            if (Params.ReturnValue != EPostHogResult::Ok) { FPlatformMisc::RequestExitWithStatus(false, 5); return false; }
        }
        bReplayStarted = true;
    }
    if (!bResized && IFileManager::Get().FileExists(*(Controls / TEXT("resize"))))
    { Viewport->Viewport->ResizeFrame(800, 450, EWindowMode::Windowed); bResized = true; }
    if (bReplayStarted && !bHalted && !bResumed && IFileManager::Get().FileExists(*(Controls / TEXT("halt"))))
    { Subsystem->StopSessionReplay(); bHalted = true; Client->Capture(TEXT("replay_smoke_halted")); }
    if (bHalted && !bResumed && IFileManager::Get().FileExists(*(Controls / TEXT("resume"))))
    { Subsystem->StartSessionReplay(); bResumed = true; }
    // All screenshots come from the production owning-viewport recorder.
    return true;
}
void UPostHogSmokeGameInstance::Shutdown()
{
    FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
    Super::Shutdown();
    if (Pattern) { if (auto* Viewport = GetGameViewportClient()) { Viewport->RemoveViewportWidgetContent(Pattern.ToSharedRef()); } }
    Pattern.Reset();
    if (OtherWindow && FSlateApplication::IsInitialized()) { FSlateApplication::Get().RequestDestroyWindow(OtherWindow.ToSharedRef()); }
    OtherWindow.Reset();
}
