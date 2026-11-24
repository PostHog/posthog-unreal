#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogReplay.h"
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogReplayMathAndPixelsTest, "PostHog.Replay.MathAndPixels",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

bool FPostHogReplayMathAndPixelsTest::RunTest(const FString&)
{
    TestEqual(TEXT("scales landscape to max dimension"), PostHogReplay::ScaledSize(1920, 1080, 960), FIntPoint(960, 540));
    TestEqual(TEXT("does not upscale smaller viewport"), PostHogReplay::ScaledSize(320, 180, 960), FIntPoint(320, 180));
    TestEqual(TEXT("rejects invalid dimensions"), PostHogReplay::ScaledSize(0, 100, 960), FIntPoint::ZeroValue);

    FIntRect Rect;
    TestTrue(TEXT("computes valid viewport rect"),
        PostHogReplay::ViewportRect(FVector2D(10, 20), FVector2D(110, 20), FVector2D(110, 70), FVector2D(200, 100), FIntPoint(400, 200), Rect));
    TestEqual(TEXT("scaled rect"), Rect, FIntRect(20, 40, 220, 140));

    // Convert a 2x2 RGBA red/green/blue/white image to BGRA8888
    TArray64<uint8> Rgba = {
        255, 0, 0, 128,   0, 255, 0, 64,
        0, 0, 255, 32,    255, 255, 255, 0
    };
    TArray64<uint8> Bgra;
    int32 OutW = 0, OutH = 0;
    TestTrue(TEXT("converts RGBA to opaque BGRA"), PostHogReplay::PrepareBgra(PF_R8G8B8A8, Rgba, 2, 2, 64, Bgra, OutW, OutH));
    TestEqual(TEXT("width"), OutW, 2);
    TestEqual(TEXT("height"), OutH, 2);
    TestEqual(TEXT("byte count"), Bgra.Num(), static_cast<int64>(16));
    // Pixel 0: red -> BGRA (0, 0, 255, 255)
    TestEqual(TEXT("red B"), Bgra[0], static_cast<uint8>(0));
    TestEqual(TEXT("red G"), Bgra[1], static_cast<uint8>(0));
    TestEqual(TEXT("red R"), Bgra[2], static_cast<uint8>(255));
    TestEqual(TEXT("red A"), Bgra[3], static_cast<uint8>(255));
    return true;
}
#endif
