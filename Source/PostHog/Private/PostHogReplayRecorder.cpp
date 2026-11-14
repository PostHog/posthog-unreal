#include "PostHogReplayRecorder.h"
#include "PostHogReplay.h"
#include "PostHogReplayWork.h"
#include "PostHogReadbackSlot.h"
#include "Containers/Ticker.h"
#include "Engine/GameInstance.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeLock.h"
#include "RHIGPUReadback.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SLeafWidget.h"
#include "Widgets/SWindow.h"
#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogPostHogReplay, Log, All);
namespace
{
FCriticalSection RegistryMutex;
TArray<FPostHogReplayRecorder::FPtr> Recorders;
bool bExiting = false;
struct FSource
{
    const SWindow* Window = nullptr;
    FIntPoint WindowPixels = FIntPoint::ZeroValue;
    FIntRect Rect;
    bool operator==(const FSource& Other) const
    { return Window == Other.Window && WindowPixels == Other.WindowPixels && Rect == Other.Rect; }
};
/** Paint-time metadata is queued before the Slate draw command for that exact window paint. */
class SPostHogViewportObserver final : public SLeafWidget
{
public:
    SLATE_BEGIN_ARGS(SPostHogViewportObserver) {} SLATE_END_ARGS()
    void Construct(const FArguments&) { SetVisibility(EVisibility::HitTestInvisible); ForceVolatile(true); }
    TFunction<void(const FGeometry&, const FSlateRect&, SWindow*)> Painted;
    virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
    virtual int32 OnPaint(const FPaintArgs&, const FGeometry& Geometry, const FSlateRect& Culling, FSlateWindowElementList& Elements,
        int32 Layer, const FWidgetStyle&, bool) const override
    { if (Painted && IsInGameThread()) { Painted(Geometry, Culling, Elements.GetPaintWindow()); } return Layer; }
};
}
struct FPostHogReplayRecorder::FImpl final : FRunnable
{
    struct FJob
    {
        TUniquePtr<FPostHogReplayWork> Work;
        TArray64<uint8> Raw;
        EPixelFormat Format = PF_Unknown;
        FIntPoint Size = FIntPoint::ZeroValue;
    };
    struct FSlot
    {
        TPostHogReadbackSlot<FRHIGPUTextureReadback> Readback;
        TUniquePtr<FJob> Job;
    };
    TWeakObjectPtr<UGameInstance> Instance;
    TWeakObjectPtr<UGameViewportClient> Viewport;
    TSharedPtr<SPostHogViewportObserver> Observer;
    FPostHogClient::FPtr Client;
    TSharedRef<FPostHogReplayWorkState, ESPMode::ThreadSafe> State = MakeShared<FPostHogReplayWorkState, ESPMode::ThreadSafe>();
    FSlateRenderer* Renderer = nullptr;
    FTSTicker::FDelegateHandle Ticker;
    FDelegateHandle BackBuffer, PaintEnd, Exit;
    TPromise<bool> DonePromise;
    TSharedFuture<bool> Done = DonePromise.GetFuture().Share();
    std::atomic<bool> bStopping{false}, bAvailable{true};
    std::atomic<bool> bStopped{false};
    int32 MaxDimension = 960;
    double Interval = 1.0;
    FRunnableThread* Thread = nullptr;
    FEvent* Wake = FPlatformProcess::GetSynchEventFromPool(false);
    FCriticalSection QueueMutex;
    TArray<TUniquePtr<FJob>> Queue;
    // These fields and GPU resources are render-thread-only.
    FSlot Slots[FPostHogReplayWorkState::Capacity];
    FSource Source;
    FSource Previous;
    bool bPaintPending = false;
    EPixelFormat PreviousFormat = PF_Unknown;
    double LastCapture = -1.e9;

    virtual uint32 Run() override
    {
        while (!bStopping.load())
        {
            Wake->Wait();
            for (;;)
            {
                TUniquePtr<FJob> Job;
                { FScopeLock Lock(&QueueMutex); if (Queue.IsEmpty()) { break; } Job = MoveTemp(Queue[0]); Queue.RemoveAt(0); }
                if (!bStopping.load() && Job->Work->IsCurrent())
                {
                    TArray64<uint8> Pixels; int32 W = 0, H = 0;
                    if (PostHogReplay::PrepareBgra(Job->Format, Job->Raw, Job->Size.X, Job->Size.Y, MaxDimension, Pixels, W, H))
                    { Job->Work->Submit(Pixels); }
                }
                Job.Reset();
            }
        }
        return 0;
    }
    void Cancel()
    {
        bStopping.store(true); State->Stop();
    }
    void Pump()
    {
        check(IsInRenderingThread());
        for (auto& Slot : Slots)
        {
            if (!Slot.Job || !Slot.Readback->IsReady()) { continue; }
            auto Job = MoveTemp(Slot.Job);
            if (bStopping.load() || !Job->Work->IsCurrent()) { continue; }
            int32 Pitch = 0, Height = 0;
            const auto* Data = static_cast<const uint8*>(Slot.Readback->Lock(Pitch, &Height));
            const bool bCopied = Data && PostHogReplay::CopyRows(Data, Pitch, Height, Job->Format, Job->Size.X, Job->Size.Y, Job->Raw);
            if (Data) { Slot.Readback->Unlock(); }
            if (!bCopied) { continue; }
            { FScopeLock Lock(&QueueMutex); Queue.Add(MoveTemp(Job)); }
            Wake->Trigger();
        }
    }
    void Ready(SWindow& Window, ISlateViewportProvider& Provider)
    {
        check(IsInRenderingThread()); Pump();
        if (!bPaintPending || Source.Window != &Window) { return; }
        bPaintPending = false;
        if (bStopping.load() || !bAvailable.load()) { return; }
        FRHITexture* Texture = Provider.GetBackBufferResource();
        if (!Texture) { return; }
        const auto& Desc = Texture->GetDesc();
        if (!PostHogReplay::MatchesSource(Source.Window, &Window, Source.WindowPixels, Desc.Extent) || Desc.NumSamples != 1 || !PostHogReplay::SupportedFormat(Desc.Format))
        { State->Cancel(); return; }
        if (!(Source == Previous) || PreviousFormat != Desc.Format)
        { State->Cancel(); Previous = Source; PreviousFormat = Desc.Format; }
        const double Now = FPlatformTime::Seconds();
        if (Now - LastCapture < Interval) { return; }
        LastCapture = Now;
        FSlot* Free = nullptr;
        for (auto& Slot : Slots) { if (!Slot.Job) { Free = &Slot; break; } }
        if (!Free) { return; }
        const FIntPoint Target = PostHogReplay::ScaledSize(Source.Rect.Width(), Source.Rect.Height(), MaxDimension);
        State->BeginCopy(*Client, Target.X, Target.Y, [&](TUniquePtr<FPostHogReplayWork> Work)
        {
            auto Job = MakeUnique<FJob>(); Job->Work = MoveTemp(Work); Job->Size = Source.Rect.Size(); Job->Format = Desc.Format;
            if (!Free->Readback.Prepare(Job->Size, Job->Format, Free->Job.IsValid(),
                [] { return MakeUnique<FRHIGPUTextureReadback>(TEXT("PostHogViewport")); })) { return; }
            auto& Cmd = FRHICommandListImmediate::Get();
            Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
            Free->Readback->EnqueueCopy(Cmd, Texture, FResolveRect(Source.Rect.Min.X, Source.Rect.Min.Y, Source.Rect.Max.X, Source.Rect.Max.Y));
            Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::Present));
            Free->Job = MoveTemp(Job);
        });
    }
    void Painted(const FGeometry& Geometry, const FSlateRect& Culling, SWindow* Window)
    {
        check(IsInGameThread());
        auto* Owner = Viewport.Get();
        if (bStopping.load() || !bAvailable.load() || !Owner || !Window || Owner->GetWindow().Get() != Window) { return; }
        // Paint geometries/culling are rooted in window space, not desktop screen space.
        const auto WindowGeometry = Window->GetWindowGeometryInWindow();
        const FVector2D Size = Geometry.GetLocalSize(); const FVector2D WindowSize = WindowGeometry.GetLocalSize();
        const FVector2D WindowPixels = Window->GetViewportSize();
        FSource Next; Next.Window = Window; Next.WindowPixels = FIntPoint(FMath::RoundToInt(WindowPixels.X), FMath::RoundToInt(WindowPixels.Y));
        const FVector2D A = Geometry.LocalToAbsolute(FVector2D::ZeroVector);
        const FVector2D B = Geometry.LocalToAbsolute(FVector2D(Size.X, 0));
        const FVector2D C = Geometry.LocalToAbsolute(Size);
        if (!FMath::IsNearlyEqual(A.Y, B.Y, 0.01) || !FMath::IsNearlyEqual(B.X, C.X, 0.01)) { return; }
        const FVector2D Min(FMath::Max(A.X, static_cast<double>(Culling.Left)), FMath::Max(A.Y, static_cast<double>(Culling.Top)));
        const FVector2D Max(FMath::Min(C.X, static_cast<double>(Culling.Right)), FMath::Min(C.Y, static_cast<double>(Culling.Bottom)));
        if (!PostHogReplay::ViewportRect(WindowGeometry.AbsoluteToLocal(Min), WindowGeometry.AbsoluteToLocal(FVector2D(Max.X, Min.Y)),
            WindowGeometry.AbsoluteToLocal(Max), WindowSize, Next.WindowPixels, Next.Rect)) { return; }
        ENQUEUE_RENDER_COMMAND(PostHogPaintSource)([this, Next](FRHICommandListImmediate&)
        { if (!(Source == Next)) { State->Cancel(); } Source = Next; bPaintPending = true; });
    }
    void BindViewport()
    {
        check(IsInGameThread());
        auto* Current = Instance.IsValid() ? Instance->GetGameViewportClient() : nullptr;
        if (Current == Viewport.Get()) { return; }
        State->Cancel();
        ENQUEUE_RENDER_COMMAND(PostHogUnbindSource)([this](FRHICommandListImmediate&) { bPaintPending = false; });
        if (auto* Old = Viewport.Get(); Old && Observer) { Old->RemoveViewportWidgetContent(Observer.ToSharedRef()); }
        Viewport = Current;
        if (Current && Observer) { Current->AddViewportWidgetContent(Observer.ToSharedRef(), 0); }
    }
    void Detach()
    {
        check(IsInGameThread());
        Cancel();
        FTSTicker::RemoveTicker(Ticker); Ticker.Reset();
        FCoreDelegates::OnEnginePreExit.Remove(Exit); Exit.Reset();
        if (auto* Old = Viewport.Get(); Old && Observer) { Old->RemoveViewportWidgetContent(Observer.ToSharedRef()); }
        Observer.Reset(); Viewport.Reset();
        if (Renderer)
        {
            Renderer->OnBackBufferReadyToPresent().Remove(BackBuffer);
            Renderer->OnSlateWindowRendered().Remove(PaintEnd);
        }
        BackBuffer.Reset(); PaintEnd.Reset();
        // Fence waits happen only during retirement, never on the normal capture path.
        ENQUEUE_RENDER_COMMAND(PostHogRetireReadbacks)([this](FRHICommandListImmediate& Cmd)
        {
            for (auto& Slot : Slots)
            {
                if (Slot.Job) { Slot.Readback->Wait(Cmd, Slot.Readback->GetLastCopyGPUMask()); }
                Slot.Job.Reset(); Slot.Readback.Reset();
            }
        });
        FlushRenderingCommands();
        Wake->Trigger();
        if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
        { FScopeLock Lock(&QueueMutex); Queue.Empty(); }
        Renderer = nullptr;
        FPlatformProcess::ReturnSynchEventToPool(Wake); Wake = nullptr;
        check(State->Pending() == 0);
        bStopped = true; DonePromise.SetValue(true);
    }
};
FPostHogReplayRecorder::FPostHogReplayRecorder() : Impl(MakeUnique<FImpl>()) {}
FPostHogReplayRecorder::~FPostHogReplayRecorder() { check(Impl->bStopped); }
FPostHogReplayRecorder::FPtr FPostHogReplayRecorder::Attach(UGameInstance* Instance, FPostHogClient::FPtr Client, const FPostHogReplaySettings& Settings)
{
    check(IsInGameThread());
    if (!Instance || !Client || !FApp::CanEverRender() || !GIsRHIInitialized || GUsingNullRHI || !FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer()) { return nullptr; }
    FPtr Recorder(new FPostHogReplayRecorder()); auto& S = *Recorder->Impl;
    S.Instance = Instance; S.Client = Client; S.MaxDimension = FMath::Max(64, Settings.MaxDimension); S.Interval = FMath::Max(100, Settings.IntervalMs) / 1000.0;
    S.Renderer = FSlateApplication::Get().GetRenderer();
    S.Thread = FRunnableThread::Create(&S, TEXT("PostHogPixels"), 0, TPri_BelowNormal);
    if (!S.Thread)
    {
        FPlatformProcess::ReturnSynchEventToPool(S.Wake); S.Wake = nullptr; S.bStopped = true; S.DonePromise.SetValue(false);
        UE_LOG(LogPostHogReplay, Warning, TEXT("Automatic viewport capture unavailable: pixel worker could not start.")); return nullptr;
    }
    const TWeakPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe> Weak = Recorder;
    if (!Client->BindHostClose([Weak] { if (auto R = Weak.Pin()) { R->Cancel(); } }, [Weak]() -> FPostHogClient::FHostClose
    {
        if (auto R = Weak.Pin()) { return R->Retire(); }
        TPromise<bool> Done; auto Future = Done.GetFuture().Share(); Done.SetValue(true); return {Future, false};
    })) { Recorder->Stop(); return nullptr; }
    S.Observer = SNew(SPostHogViewportObserver);
    S.Observer->Painted = [&S](const FGeometry& Geometry, const FSlateRect& Culling, SWindow* Window) { S.Painted(Geometry, Culling, Window); };
    S.BindViewport();
    S.BackBuffer = S.Renderer->OnBackBufferReadyToPresent().AddRaw(&S, &FImpl::Ready);
    S.PaintEnd = S.Renderer->OnSlateWindowRendered().AddLambda([&S](SWindow& Window)
    {
        // Expire even an unpresented paint; stale metadata cannot authorize a later window draw.
        const SWindow* PaintedWindow = &Window;
        ENQUEUE_RENDER_COMMAND(PostHogExpirePaint)([&S, PaintedWindow](FRHICommandListImmediate&)
        { if (S.Source.Window == PaintedWindow) { S.bPaintPending = false; } });
    });
    S.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
    {
        auto R = Weak.Pin(); if (!R) { return false; }
        if (R->Impl->bStopping.load() || !R->Impl->Instance.IsValid()) { R->Stop(); return false; }
        R->Impl->BindViewport();
        auto* FrameState = R->Impl.Get();
        ENQUEUE_RENDER_COMMAND(PostHogExpireUnpresentedPaint)([FrameState](FRHICommandListImmediate&) { FrameState->bPaintPending = false; });
        if (R->Impl->State->Pending() > 0)
        { auto* State = R->Impl.Get(); ENQUEUE_RENDER_COMMAND(PostHogPumpReadbacks)([State](FRHICommandListImmediate&) { State->Pump(); }); }
        return true;
    }));
    S.Exit = FCoreDelegates::OnEnginePreExit.AddLambda([Weak] { if (auto R = Weak.Pin()) { R->Stop(); } });
    bool bShuttingDown;
    { FScopeLock Lock(&RegistryMutex); bShuttingDown = bExiting; if (!bShuttingDown) { Recorders.Add(Recorder); } }
    if (bShuttingDown) { Recorder->Stop(); return nullptr; }
    return Recorder;
}
void FPostHogReplayRecorder::Cancel() { Impl->Cancel(); }
bool FPostHogReplayRecorder::ShouldDeferRetirement(const TSharedFuture<bool>& Done)
{ return !Done.IsReady() && IsInAnyRenderingThread(); }
TPair<TSharedFuture<bool>, bool> FPostHogReplayRecorder::Retire()
{
    if (Impl->Done.IsReady()) { return {Impl->Done, false}; }
    if (IsInGameThread()) { Stop(); }
    return {Impl->Done, ShouldDeferRetirement(Impl->Done)};
}
void FPostHogReplayRecorder::SetAvailable(bool bAvailable)
{
    check(IsInGameThread());
    if (Impl->bAvailable.exchange(bAvailable) != bAvailable)
    {
        Impl->State->Cancel(); auto* S = Impl.Get();
        ENQUEUE_RENDER_COMMAND(PostHogAvailabilityChanged)([S](FRHICommandListImmediate&) { S->bPaintPending = false; });
    }
}
void FPostHogReplayRecorder::Stop()
{
    check(IsInGameThread()); if (Impl->bStopped) { return; }
    Impl->Detach();
    FScopeLock Lock(&RegistryMutex); Recorders.RemoveAll([this](const auto& R) { return R.Get() == this; });
}
void FPostHogReplayRecorder::StopAll()
{
    check(IsInGameThread()); TArray<FPtr> Live;
    { FScopeLock Lock(&RegistryMutex); bExiting = true; Live = Recorders; }
    for (const auto& R : Live) { R->Stop(); }
}
