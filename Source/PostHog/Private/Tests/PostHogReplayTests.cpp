#if WITH_DEV_AUTOMATION_TESTS
#include "PostHogTestHelpers.h"
#include "PostHogReplay.h"
#include "PostHogReplayWork.h"
#include "PostHogReadbackSlot.h"
#include "Math/Float16Color.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogReplayTest, "PostHog.ReplayPreparedPixels", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogReplayTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors; auto Check = [&Errors](bool Pass, const TCHAR* Message) { if (!Pass) { Errors.Add(Message); } };
        auto Config = PostHogTests::Config(TEXT("adapter-replay")); Config.Replay.MaxFramesInFlight = 1;
        auto Client = FPostHogClient::CreateAsync(Config).Get(); if (!Client) { Errors.Add(TEXT("replay client creation")); return Errors; }
        Client->FlushAsync().Get(); Check(!Client->IsSessionReplayActive(), TEXT("no recording before manual start")); Client->StartSessionReplay();
        TUniquePtr<FPostHogReplayFrame> Frame, Other;
        Check(Client->BeginReplayFrame(32, 24, 128, Frame) == EPostHogResult::Ok, TEXT("acquire core reservation"));
        if (Frame)
        {
            Check(Client->BeginReplayFrame(32, 24, 128, Other) == EPostHogResult::Inactive, TEXT("held lease reserves core capacity"));
            Frame.Reset(); Check(Client->BeginReplayFrame(32, 24, 128, Frame) == EPostHogResult::Ok, TEXT("RAII abandonment releases capacity"));
            Client->StopSessionReplay();
            TArray<uint8> Pixels; Pixels.SetNumZeroed(128 * 24);
            for (int32 Row = 0; Row < 24; ++Row) { for (int32 X = 0; X < 16; ++X) { Pixels[(Row * 32 + X) * 4] = 255; } }
            Check(Frame && Frame->Submit(Pixels.GetData(), Pixels.Num()) == EPostHogResult::Accepted, TEXT("valid outstanding masked synthetic pixels may complete after stop"));
            Check(Frame && Frame->Submit(Pixels.GetData(), Pixels.Num()) == EPostHogResult::Closed, TEXT("lease consumed once"));
        }
        Client->FlushAsync().Get(); Client->StartSessionReplay();
        Client->SetAppInBackground(true);
        Check(Client->BeginReplayFrame(32, 24, 128, Frame) == EPostHogResult::Inactive, TEXT("engine foreground gate"));
        Client->SetAppInBackground(false); Check(Client->BeginReplayFrame(32, 24, 128, Frame) == EPostHogResult::Ok, TEXT("foreground acquisition"));
        Check(Client->ShutdownAsync().Get().bJoined, TEXT("close with held frame"));
        Frame.Reset(); // The core already invalidated its token; no dangling pixel pointer exists.
        return Errors;
    });
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogPixelTest, "PostHog.PixelConversion", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogPixelTest::RunTest(const FString&)
{
    TArray64<uint8> Source; Source.Append({255, 0, 0, 1, 0, 0, 0, 255});
    TArray64<uint8> Pixels; int32 Width = 0, Height = 0;
    TestTrue(TEXT("RGBA conversion"), PostHogReplay::PrepareBgra(PF_R8G8B8A8, Source, 2, 1, 2, Pixels, Width, Height));
    TestTrue(TEXT("composited BGRA colors and prepared black pixels"), Pixels.Num() == 8 && Pixels[2] == 255 && Pixels[3] == 255 && Pixels[4] == 0);
    TestTrue(TEXT("downscale remains host work"), PostHogReplay::PrepareBgra(PF_B8G8R8A8, Source, 2, 1, 1, Pixels, Width, Height) && Width == 1 && Height == 1);
    FIntRect Rect;
    TestTrue(TEXT("own viewport is cropped in scaled window space"), PostHogReplay::ViewportRect(FVector2D(100, 50), FVector2D(500, 50), FVector2D(500, 350),
        FVector2D(800, 600), FIntPoint(1600, 1200), Rect) && Rect == FIntRect(200, 100, 1000, 700));
    TestFalse(TEXT("rotated viewport is unsupported"), PostHogReplay::ViewportRect(FVector2D(0, 0), FVector2D(10, 1), FVector2D(10, 10), FVector2D(20, 20), FIntPoint(20, 20), Rect));
    TestFalse(TEXT("viewport cannot extend into another surface"), PostHogReplay::ViewportRect(FVector2D(-1, 0), FVector2D(10, 0), FVector2D(10, 10), FVector2D(20, 20), FIntPoint(20, 20), Rect));
    int Window = 0, OtherWindow = 0;
    TestTrue(TEXT("exact source window"), PostHogReplay::MatchesSource(&Window, &Window, FIntPoint(640, 480), FIntPoint(640, 480)));
    TestFalse(TEXT("unrelated window rejected"), PostHogReplay::MatchesSource(&Window, &OtherWindow, FIntPoint(640, 480), FIntPoint(640, 480)));
    TestFalse(TEXT("resize invalidates paint dimensions"), PostHogReplay::MatchesSource(&Window, &Window, FIntPoint(640, 480), FIntPoint(800, 600)));
    const uint8 Pitched[] = {1,2,3,4, 0,0,0,0, 5,6,7,8, 0,0,0,0};
    TestTrue(TEXT("pitch padding removed in top-down row order"), PostHogReplay::CopyRows(Pitched, 2, 2, PF_B8G8R8A8, 1, 2, Pixels) &&
        Pixels.Num() == 8 && Pixels[0] == 1 && Pixels[4] == 5);
    TestFalse(TEXT("short pitch rejected"), PostHogReplay::CopyRows(Pitched, 1, 2, PF_B8G8R8A8, 2, 2, Pixels));
    TestFalse(TEXT("short mapped height rejected"), PostHogReplay::CopyRows(Pitched, 2, 1, PF_B8G8R8A8, 1, 2, Pixels));
    TestFalse(TEXT("pitch arithmetic overflow rejected before allocation"), PostHogReplay::CopyRows(Pitched, MAX_int32, 0, PF_FloatRGBA, 1, MAX_int32, Pixels));
    TestFalse(TEXT("unsupported format rejected"), PostHogReplay::CopyRows(Pitched, 2, 2, PF_G8, 1, 2, Pixels));
    uint32 Packed = 1023; Source.Empty(); Source.Append(reinterpret_cast<const uint8*>(&Packed), sizeof(Packed));
    TestTrue(TEXT("10-bit conversion"), PostHogReplay::PrepareBgra(PF_A2B10G10R10, Source, 1, 1, 64, Pixels, Width, Height) && Pixels[2] == 255 && Pixels[0] == 0);
    FFloat16Color Half(FLinearColor::Green); Source.Empty(); Source.Append(reinterpret_cast<const uint8*>(&Half), sizeof(Half));
    TestTrue(TEXT("HDR conversion"), PostHogReplay::PrepareBgra(PF_FloatRGBA, Source, 1, 1, 64, Pixels, Width, Height) && Pixels[1] == 255 && Pixels[3] == 255);
    TestTrue(TEXT("aspect ratio and host default size"), PostHogReplay::ScaledSize(1920, 1080, 960) == FIntPoint(960, 540));

    struct FTrackedReadback
    {
        int32 Id; FIntPoint Extent; EPixelFormat Format;
        int32& Destroyed; int32& DestroyedPending; bool bPending = false;
        FTrackedReadback(int32 InId, FIntPoint InExtent, EPixelFormat InFormat, int32& InDestroyed, int32& InDestroyedPending)
            : Id(InId), Extent(InExtent), Format(InFormat), Destroyed(InDestroyed), DestroyedPending(InDestroyedPending) {}
        ~FTrackedReadback() { ++Destroyed; if (bPending) { ++DestroyedPending; } }
    };
    int32 Created = 0, Destroyed = 0, DestroyedPending = 0;
    TPostHogReadbackSlot<FTrackedReadback> Allocation;
    auto Prepare = [&](FIntPoint Extent, EPixelFormat Format)
    {
        return Allocation.Prepare(Extent, Format, Allocation.Get() && Allocation->bPending,
            [&] { return MakeUnique<FTrackedReadback>(++Created, Extent, Format, Destroyed, DestroyedPending); });
    };
    TestTrue(TEXT("initial crop allocates readback"), Prepare(FIntPoint(640, 480), PF_B8G8R8A8) && Created == 1 && Destroyed == 0);
    for (int32 Frame = 0; Frame < 5; ++Frame) { TestTrue(TEXT("compatible readback reused"), Prepare(FIntPoint(640, 480), PF_B8G8R8A8) && Allocation->Id == 1); }
    TestEqual(TEXT("compatible frames do not recreate"), Created, 1);
    TestTrue(TEXT("crop growth replaces retired allocation"), Prepare(FIntPoint(800, 450), PF_B8G8R8A8) &&
        Allocation->Extent == FIntPoint(800, 450) && Created == 2 && Destroyed == 1);
    TestTrue(TEXT("crop shrink replaces retired allocation"), Prepare(FIntPoint(320, 240), PF_B8G8R8A8) && Created == 3 && Destroyed == 2);
    TestTrue(TEXT("format change replaces retired allocation"), Prepare(FIntPoint(320, 240), PF_FloatRGBA) &&
        Allocation->Format == PF_FloatRGBA && Created == 4 && Destroyed == 3);
    Allocation->bPending = true;
    TestFalse(TEXT("pending copy blocks crop growth"), Prepare(FIntPoint(800, 450), PF_FloatRGBA));
    TestFalse(TEXT("pending copy blocks crop shrink"), Prepare(FIntPoint(160, 120), PF_FloatRGBA));
    TestFalse(TEXT("pending copy blocks format replacement"), Prepare(FIntPoint(320, 240), PF_B8G8R8A8));
    TestFalse(TEXT("pending copy blocks compatible re-enqueue"), Prepare(FIntPoint(320, 240), PF_FloatRGBA));
    TestTrue(TEXT("outstanding allocation preserved"), Allocation->Id == 4 && Allocation->Extent == FIntPoint(320, 240) &&
        Allocation->Format == PF_FloatRGBA && Created == 4 && Destroyed == 3 && DestroyedPending == 0);
    Allocation->bPending = false; // Models Pump observing the previous GPU fence ready.
    TestTrue(TEXT("rejected changes leave compatible key intact"), Prepare(FIntPoint(320, 240), PF_FloatRGBA) && Created == 4);
    TestTrue(TEXT("deferred replacement occurs after fence retirement"), Prepare(FIntPoint(800, 450), PF_B8G8R8A8) && Created == 5 && Destroyed == 4);
    Allocation.Reset();
    TestTrue(TEXT("readbacks retire exactly once without pending destruction"), Destroyed == Created && DestroyedPending == 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPostHogStagingTest, "PostHog.ReplayStaging", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPostHogStagingTest::RunTest(const FString&)
{
    return PostHogTests::Run(this, []
    {
        TArray<FString> Errors; auto Check = [&Errors](bool Pass, const TCHAR* Message) { if (!Pass) { Errors.Add(Message); } };
        auto Config = PostHogTests::Config(TEXT("replay-staging")); Config.Replay.MaxFramesInFlight = 8;
        auto Client = FPostHogClient::CreateAsync(Config).Get(); if (!Client) { Errors.Add(TEXT("create")); return Errors; }
        Client->FlushAsync().Get();
        auto State = MakeShared<FPostHogReplayWorkState, ESPMode::ThreadSafe>();
        TUniquePtr<FPostHogReplayWork> First, Second; int32 Copies = 0;
        auto CopyFirst = [&](TUniquePtr<FPostHogReplayWork> Work) { ++Copies; First = MoveTemp(Work); };
        Check(!State->BeginCopy(*Client, 32, 24, CopyFirst) && Copies == 0 && State->Pending() == 0, TEXT("no acquisition before native recording admission"));
        Client->StartSessionReplay();
        Check(State->BeginCopy(*Client, 32, 24, CopyFirst) && Copies == 1 && State->Pending() == 1, TEXT("native reservation precedes acquisition callback"));
        Check(State->BeginCopy(*Client, 32, 24, [&](TUniquePtr<FPostHogReplayWork> Work) { ++Copies; Second = MoveTemp(Work); }), TEXT("second host slot"));
        Check(!State->BeginCopy(*Client, 32, 24, CopyFirst) && Copies == 2, TEXT("host backlog bounded independently of native capacity"));
        TArray64<uint8> Pixels; Pixels.SetNumZeroed(32 * 24 * 4);
        Client->StopSessionReplay();
        if (First) { Check(First->Submit(Pixels) == EPostHogResult::Accepted && First->Submit(Pixels) == EPostHogResult::Closed, TEXT("exact lease submits once after manual stop")); }
        First.Reset(); Check(State->Pending() == 1, TEXT("completed work releases host slot"));
        State->Cancel();
        if (Second) { Check(!Second->IsCurrent() && Second->Submit(Pixels) == EPostHogResult::Inactive, TEXT("engine source/resize cancellation discards old work")); }
        Second.Reset(); Client->StartSessionReplay();
        First = State->Begin(*Client, 32, 24); Check(First.IsValid(), TEXT("new source can acquire"));
        State->Stop(); Check(Client->ShutdownAsync().Get().bJoined, TEXT("one shutdown operation"));
        if (First) { Check(First->Submit(Pixels) == EPostHogResult::Inactive, TEXT("late completion discarded after shutdown")); }
        First.Reset(); Check(State->Pending() == 0 && !State->Begin(*Client, 32, 24), TEXT("all host permits retired"));
        return Errors;
    });
}
#endif
