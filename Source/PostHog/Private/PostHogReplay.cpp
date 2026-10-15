#include "PostHogReplay.h"
#include "PostHogJson.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "ImageCore.h"
#include "ImageUtils.h"
#include "Math/Float16Color.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "RHIGPUReadback.h"
#include "Rendering/SlateRenderer.h"
#include "RenderingThread.h"
#include "Slate/SlateViewportProvider.h"
#include "Widgets/SWindow.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogReplay, Log, All);

struct FPostHogReplayRecorder::FSlot
{
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    FDateTime CapturedAt;
    bool bPending = false;
};

struct FPostHogReplayRecorder::FWorker final : public FRunnable
{
    FPostHogReplayRecorder& Owner;
    explicit FWorker(FPostHogReplayRecorder& InOwner) : Owner(InOwner) {}
    virtual uint32 Run() override { Owner.RunWorker(); return 0; }
};

namespace
{
constexpr int32 RingSlots = 3;
constexpr int32 MaxQueuedFrames = 2;
constexpr uint32 WorkerWaitMs = 100;
constexpr int32 LogEvery = 10;

FCriticalSection RecordersMutex;
TArray<TWeakPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe>> Recorders;
bool bRecordersStopping = false;

bool IsSupportedFormat(EPixelFormat Format)
{
    return Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8 || Format == PF_A2B10G10R10 || Format == PF_FloatRGBA;
}

/** Non-BGRA8 formats only; BGRA8 frames are used in place. */
bool ToBgra8(EPixelFormat Format, const uint8* Source, int64 Count, FColor* Out)
{
    switch (Format)
    {
    case PF_R8G8B8A8:
        for (int64 Index = 0; Index < Count; ++Index, Source += 4) { Out[Index] = FColor(Source[0], Source[1], Source[2]); }
        break;
    case PF_A2B10G10R10:
        for (int64 Index = 0; Index < Count; ++Index, Source += 4)
        {
            uint32 Packed; FMemory::Memcpy(&Packed, Source, sizeof(Packed));
            Out[Index] = FColor::MakeRequantizeFrom1010102(Packed & 0x3FF, (Packed >> 10) & 0x3FF, (Packed >> 20) & 0x3FF, 3);
        }
        break;
    case PF_FloatRGBA:
        for (int64 Index = 0; Index < Count; ++Index, Source += sizeof(FFloat16Color))
        {
            FFloat16Color Half; FMemory::Memcpy(&Half, Source, sizeof(Half));
            Out[Index] = FLinearColor(Half).ToFColorSRGB();
        }
        break;
    default:
        return false;
    }
    for (int64 Index = 0; Index < Count; ++Index) { Out[Index].A = 255; }
    return true;
}
}

FString PostHogReplay::BuildSnapshotEnvelope(const FString& DistinctId, const FString& SessionId, int32 Width, int32 Height,
    const TArray64<uint8>& Jpeg, const FDateTime& CapturedAt)
{
    const double Timestamp = static_cast<double>(PostHogJson::UnixMilliseconds(CapturedAt));
    auto Meta = MakeShared<FJsonObject>();
    Meta->SetNumberField(TEXT("width"), Width); Meta->SetNumberField(TEXT("height"), Height);
    Meta->SetStringField(TEXT("href"), FApp::GetProjectName());
    auto MetaEvent = MakeShared<FJsonObject>();
    MetaEvent->SetNumberField(TEXT("type"), 4); MetaEvent->SetNumberField(TEXT("timestamp"), Timestamp); MetaEvent->SetObjectField(TEXT("data"), Meta);
    auto Offset = MakeShared<FJsonObject>();
    Offset->SetNumberField(TEXT("top"), 0); Offset->SetNumberField(TEXT("left"), 0);
    auto Wireframe = MakeShared<FJsonObject>();
    Wireframe->SetNumberField(TEXT("id"), 1); Wireframe->SetNumberField(TEXT("x"), 0); Wireframe->SetNumberField(TEXT("y"), 0);
    Wireframe->SetNumberField(TEXT("width"), Width); Wireframe->SetNumberField(TEXT("height"), Height);
    Wireframe->SetStringField(TEXT("type"), TEXT("screenshot"));
    Wireframe->SetStringField(TEXT("base64"), TEXT("data:image/jpeg;base64,") + FBase64::Encode(Jpeg.GetData(), static_cast<uint32>(Jpeg.Num())));
    auto Full = MakeShared<FJsonObject>();
    Full->SetObjectField(TEXT("initialOffset"), Offset);
    TArray<TSharedPtr<FJsonValue>> Wireframes; Wireframes.Add(MakeShared<FJsonValueObject>(Wireframe));
    Full->SetArrayField(TEXT("wireframes"), Wireframes);
    auto FullEvent = MakeShared<FJsonObject>();
    FullEvent->SetNumberField(TEXT("type"), 2); FullEvent->SetNumberField(TEXT("timestamp"), Timestamp); FullEvent->SetObjectField(TEXT("data"), Full);
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("$snapshot_source"), TEXT("mobile"));
    Properties->SetStringField(TEXT("$session_id"), SessionId);
    Properties->SetStringField(TEXT("$window_id"), SessionId);
    Properties->SetStringField(TEXT("$lib"), PostHogJson::LibName);
    Properties->SetStringField(TEXT("$lib_version"), PostHogJson::LibVersion);
    TArray<TSharedPtr<FJsonValue>> SnapshotData;
    SnapshotData.Add(MakeShared<FJsonValueObject>(MetaEvent)); SnapshotData.Add(MakeShared<FJsonValueObject>(FullEvent));
    Properties->SetArrayField(TEXT("$snapshot_data"), SnapshotData);
    auto Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("event"), TEXT("$snapshot"));
    Envelope->SetStringField(TEXT("distinct_id"), DistinctId);
    Envelope->SetStringField(TEXT("uuid"), PostHogJson::NewUuidV7(CapturedAt));
    Envelope->SetStringField(TEXT("timestamp"), CapturedAt.ToIso8601());
    Envelope->SetObjectField(TEXT("properties"), Properties);
    return PostHogJson::Encode(Envelope);
}

FPostHogReplayRecorder::FPostHogReplayRecorder(FPostHogClient::FPtr InClient, const FPostHogReplaySettings& Settings)
    : Client(MoveTemp(InClient))
    , IntervalSeconds(FMath::Max(Settings.IntervalMs, 100) / 1000.0)
    , MaxDimension(FMath::Max(Settings.MaxDimension, 64))
    , JpegQuality(FMath::Clamp(Settings.JpegQuality, 1, 100))
{
}

FPostHogReplayRecorder::FPtr FPostHogReplayRecorder::Start(FPostHogClient::FPtr Client, const FPostHogReplaySettings& Settings)
{
    check(IsInGameThread());
    if (!Client) { return nullptr; }
    if (!FApp::CanEverRender())
    { UE_LOG(LogPostHogReplay, Log, TEXT("Session replay disabled: this process does not render.")); return nullptr; }
    if (!FSlateApplication::IsInitialized() || !FSlateApplication::Get().GetRenderer())
    { UE_LOG(LogPostHogReplay, Log, TEXT("Session replay disabled: Slate rendering is not available.")); return nullptr; }
    if (GIsEditor)
    { UE_LOG(LogPostHogReplay, Warning, TEXT("Session replay in the editor captures the whole editor window. Use -game for game-only frames.")); }

    FPtr Recorder(new FPostHogReplayRecorder(MoveTemp(Client), Settings));
    Recorder->ImageWrapper = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
    if (GEngine && GEngine->GameViewport)
    {
        if (const TSharedPtr<SWindow> Window = GEngine->GameViewport->GetWindow()) { Recorder->TargetWindow.store(Window.Get()); }
    }
    {
        FScopeLock Lock(&RecordersMutex);
        if (bRecordersStopping) { Recorder->bStopping.store(true); return nullptr; }
        Recorders.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
        Recorders.Add(Recorder);
    }
    Recorder->Wake = FPlatformProcess::GetSynchEventFromPool(false);
    Recorder->Worker = MakeUnique<FWorker>(*Recorder);
    Recorder->Thread = FRunnableThread::Create(Recorder->Worker.Get(), TEXT("PostHogReplay"), 0, TPri_BelowNormal);
    if (!Recorder->Thread)
    {
        UE_LOG(LogPostHogReplay, Warning, TEXT("Session replay disabled: could not start the encoder thread."));
        Recorder->Stop();
        return nullptr;
    }
    Recorder->BackBufferHandle = FSlateApplication::Get().GetRenderer()->OnBackBufferReadyToPresent().AddRaw(Recorder.Get(), &FPostHogReplayRecorder::OnBackBufferReady);
    UE_LOG(LogPostHogReplay, Log, TEXT("Session replay started: interval %d ms, max dimension %d px, JPEG quality %d, window %s."),
        FMath::RoundToInt(Recorder->IntervalSeconds * 1000.0), Recorder->MaxDimension, Recorder->JpegQuality,
        Recorder->TargetWindow.load() ? TEXT("game viewport") : TEXT("first regular window"));
    return Recorder;
}

void FPostHogReplayRecorder::StopAll()
{
    TArray<FPtr> Live;
    {
        FScopeLock Lock(&RecordersMutex);
        bRecordersStopping = true;
        for (const auto& Weak : Recorders) { if (auto Recorder = Weak.Pin()) { Live.Add(MoveTemp(Recorder)); } }
        Recorders.Empty();
    }
    for (const auto& Recorder : Live) { Recorder->Stop(); }
}

FPostHogReplayRecorder::~FPostHogReplayRecorder()
{
    // Owners stop on the game thread; this only covers an owner that forgot to.
    if (!bStopping.load() && ensureMsgf(IsInGameThread(), TEXT("PostHog replay recorder destroyed off the game thread without Stop."))) { Stop(); }
}

void FPostHogReplayRecorder::Stop()
{
    if (bStopping.exchange(true)) { return; }
    check(IsInGameThread());
    if (BackBufferHandle.IsValid() && FSlateApplication::IsInitialized())
    {
        if (FSlateRenderer* Renderer = FSlateApplication::Get().GetRenderer()) { Renderer->OnBackBufferReadyToPresent().Remove(BackBufferHandle); }
    }
    BackBufferHandle.Reset();
    if (GIsRHIInitialized)
    {
        // The flush also waits for a hook invocation that was already running when the delegate was removed.
        ENQUEUE_RENDER_COMMAND(PostHogReplayRelease)([this](FRHICommandListImmediate&) { Slots.Empty(); });
        FlushRenderingCommands();
    }
    else
    {
        // Releasing RHI resources after RHI shutdown is unsafe; the process is exiting.
        for (auto& Slot : Slots) { (void)Slot->Readback.Release(); }
        Slots.Empty();
    }
    if (Wake) { Wake->Trigger(); }
    if (Thread) { Thread->WaitForCompletion(); delete Thread; Thread = nullptr; }
    if (Wake) { FPlatformProcess::ReturnSynchEventToPool(Wake); Wake = nullptr; }
    int64 Discarded = 0;
    { FScopeLock Lock(&QueueMutex); Discarded = Queue.Num(); Queue.Empty(); FreeBuffers.Empty(); }
    {
        FScopeLock Lock(&RecordersMutex);
        Recorders.RemoveAll([this](const auto& Weak) { const auto Pinned = Weak.Pin(); return !Pinned || Pinned.Get() == this; });
    }
    UE_LOG(LogPostHogReplay, Log,
        TEXT("Session replay stopped: captured %lld, dropped (no slot) %lld, dropped (backlog) %lld, dropped (gated) %lld, encode failed %lld, discarded at stop %lld; results accepted %lld, disabled %lld, invalid %lld, queue full %lld, closed %lld."),
        Captured.load(), DroppedNoSlot.load(), DroppedBacklog.load(), DroppedGated.load(), EncodeFailed.load(), Discarded,
        Results[static_cast<uint8>(EPostHogCaptureResult::Accepted)], Results[static_cast<uint8>(EPostHogCaptureResult::Disabled)],
        Results[static_cast<uint8>(EPostHogCaptureResult::Invalid)], Results[static_cast<uint8>(EPostHogCaptureResult::QueueFull)],
        Results[static_cast<uint8>(EPostHogCaptureResult::Closed)]);
}

void FPostHogReplayRecorder::OnBackBufferReady(SWindow& Window, ISlateViewportProvider& Provider)
{
    if (bStopping.load(std::memory_order_acquire) || bPaused.load(std::memory_order_acquire)) { return; }
    const SWindow* Target = TargetWindow.load();
    if (!Target)
    {
        if (!Window.IsRegularWindow()) { return; }
        TargetWindow.compare_exchange_strong(Target, &Window);
        Target = TargetWindow.load();
    }
    if (&Window != Target) { return; }

    bool bWorked = false;
    for (const auto& Slot : Slots) { bWorked |= Slot->bPending; }
    const double Start = FPlatformTime::Seconds();
    const bool bCaptureDue = Start - LastCaptureSeconds >= IntervalSeconds && bGateOpen.load(std::memory_order_acquire);
    if (!bWorked && !bCaptureDue) { return; }
    DrainReadySlots();
    if (bCaptureDue)
    {
        LastCaptureSeconds = Start;
        FRHITexture* Texture = Provider.GetBackBufferResource();
        const FRHITextureDesc* Desc = Texture ? &Texture->GetDesc() : nullptr;
        if (Desc && Desc->NumSamples == 1 && !IsSupportedFormat(Desc->Format))
        {
            if (!bWarnedFormat.exchange(true))
            { UE_LOG(LogPostHogReplay, Warning, TEXT("Session replay skipped: unsupported back buffer format %s."), GetPixelFormatString(Desc->Format)); }
        }
        else if (Desc && Desc->NumSamples == 1)
        {
            if (Desc->Extent != RingSize || Desc->Format != RingFormat)
            {
                Slots.Reset();
                for (int32 Index = 0; Index < RingSlots; ++Index)
                {
                    auto Slot = MakeUnique<FSlot>();
                    Slot->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("PostHogReplay"));
                    Slots.Add(MoveTemp(Slot));
                }
                RingSize = Desc->Extent; RingFormat = Desc->Format;
            }
            FSlot* Free = nullptr;
            for (const auto& Slot : Slots) { if (!Slot->bPending) { Free = Slot.Get(); break; } }
            if (!Free) { DroppedNoSlot.fetch_add(1); }
            else
            {
                FRHICommandListImmediate& RHICmdList = FRHICommandListImmediate::Get();
                RHICmdList.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
                Free->Readback->EnqueueCopy(RHICmdList, Texture);
                Free->CapturedAt = FDateTime::UtcNow();
                Free->bPending = true;
                Captured.fetch_add(1);
                bWorked = true;
            }
        }
    }
    if (bWorked)
    {
        RenderNanoseconds.fetch_add(static_cast<int64>((FPlatformTime::Seconds() - Start) * 1.0e9));
        RenderSamples.fetch_add(1);
    }
}

void FPostHogReplayRecorder::DrainReadySlots()
{
    const int32 BytesPerPixel = GPixelFormats[RingFormat].BlockBytes;
    for (const auto& Slot : Slots)
    {
        if (!Slot->bPending || !Slot->Readback->IsReady()) { continue; }
        Slot->bPending = false;
        int32 PitchPixels = 0, BufferHeight = 0;
        // Lock maps through the immediate command list, which requires the render thread.
        const uint8* Data = static_cast<const uint8*>(Slot->Readback->Lock(PitchPixels, &BufferHeight));
        if (!Data) { continue; }
        if (PitchPixels >= RingSize.X && (BufferHeight == 0 || BufferHeight >= RingSize.Y))
        {
            FFrame Frame;
            Frame.Format = RingFormat; Frame.Width = RingSize.X; Frame.Height = RingSize.Y; Frame.CapturedAt = Slot->CapturedAt;
            { FScopeLock Lock(&QueueMutex); if (!FreeBuffers.IsEmpty()) { Frame.Pixels = FreeBuffers.Pop(EAllowShrinking::No); } }
            const int64 RowBytes = static_cast<int64>(RingSize.X) * BytesPerPixel;
            const int64 SourcePitch = static_cast<int64>(PitchPixels) * BytesPerPixel;
            Frame.Pixels.SetNumUninitialized(RowBytes * RingSize.Y, EAllowShrinking::No);
            if (PitchPixels == RingSize.X) { FMemory::Memcpy(Frame.Pixels.GetData(), Data, RowBytes * RingSize.Y); }
            else
            {
                for (int32 Row = 0; Row < RingSize.Y; ++Row)
                { FMemory::Memcpy(Frame.Pixels.GetData() + Row * RowBytes, Data + Row * SourcePitch, RowBytes); }
            }
            {
                FScopeLock Lock(&QueueMutex);
                if (Queue.Num() >= MaxQueuedFrames)
                {
                    RecycleLocked(MoveTemp(Queue[0].Pixels));
                    Queue.RemoveAt(0);
                    DroppedBacklog.fetch_add(1);
                }
                Queue.Add(MoveTemp(Frame));
            }
            Wake->Trigger();
        }
        Slot->Readback->Unlock();
    }
}

void FPostHogReplayRecorder::RunWorker()
{
    while (!bStopping.load(std::memory_order_acquire))
    {
        Wake->Wait(WorkerWaitMs);
        if (bStopping.load(std::memory_order_acquire)) { break; }
        RefreshGate();
        TArray<FFrame> Frames;
        { FScopeLock Lock(&QueueMutex); Swap(Frames, Queue); }
        for (const FFrame& Frame : Frames)
        {
            if (bStopping.load(std::memory_order_acquire)) { break; }
            Submit(Frame);
        }
        FScopeLock Lock(&QueueMutex);
        for (FFrame& Frame : Frames) { RecycleLocked(MoveTemp(Frame.Pixels)); }
    }
}

void FPostHogReplayRecorder::RecycleLocked(TArray64<uint8>&& Pixels)
{
    if (FreeBuffers.Num() < RingSlots + MaxQueuedFrames) { FreeBuffers.Add(MoveTemp(Pixels)); }
}

void FPostHogReplayRecorder::RefreshGate()
{
    bGateOpen.store(Client->IsSessionReplayAllowed() && !Client->PeekSessionId().IsEmpty(), std::memory_order_release);
}

void FPostHogReplayRecorder::Submit(const FFrame& Frame)
{
    const FString SessionId = Client->PeekSessionId();
    if (SessionId.IsEmpty() || !Client->IsSessionReplayAllowed()) { DroppedGated.fetch_add(1); return; }
    const double Start = FPlatformTime::Seconds();
    TArray64<uint8> Jpeg;
    int32 ScaledWidth = 0, ScaledHeight = 0;
    if (!Encode(Frame, Jpeg, ScaledWidth, ScaledHeight))
    {
        EncodeFailed.fetch_add(1);
        if (!bWarnedEncode.exchange(true))
        { UE_LOG(LogPostHogReplay, Warning, TEXT("Session replay could not encode a %dx%d %s frame; dropping it."), Frame.Width, Frame.Height, GetPixelFormatString(Frame.Format)); }
        return;
    }
    WorkerSeconds += FPlatformTime::Seconds() - Start; ++WorkerSamples;
    LastJpegBytes = Jpeg.Num();
    const EPostHogCaptureResult Result = Client->SubmitReplaySnapshot(SessionId, Frame.Width, Frame.Height, Jpeg, Frame.CapturedAt);
    const int64 Count = ++Results[static_cast<uint8>(Result)];
    if (Result != EPostHogCaptureResult::Accepted)
    {
        if (!bWarnedResult.exchange(true))
        { UE_LOG(LogPostHogReplay, Warning, TEXT("Session replay frame was not accepted: %s."), *UEnum::GetValueAsString(Result)); }
        return;
    }
    if (Count % LogEvery == 0)
    {
        const int64 Samples = RenderSamples.load();
        UE_LOG(LogPostHogReplay, Log, TEXT("Session replay: %lld frames submitted; render thread %.3f ms avg, worker %.2f ms avg, last JPEG %lld bytes, %dx%d scaled to %dx%d."),
            Count, Samples > 0 ? RenderNanoseconds.load() / 1.0e6 / Samples : 0.0, WorkerSamples > 0 ? WorkerSeconds * 1000.0 / WorkerSamples : 0.0,
            LastJpegBytes, Frame.Width, Frame.Height, ScaledWidth, ScaledHeight);
    }
}

bool FPostHogReplayRecorder::Encode(const FFrame& Frame, TArray64<uint8>& OutJpeg, int32& OutWidth, int32& OutHeight) const
{
    const int64 Count = static_cast<int64>(Frame.Width) * Frame.Height;
    if (Count <= 0 || Count > MAX_int32 || Frame.Pixels.Num() < Count * GPixelFormats[Frame.Format].BlockBytes) { return false; }
    TArray<FColor> Converted;
    const FColor* Colors = reinterpret_cast<const FColor*>(Frame.Pixels.GetData());
    if (Frame.Format != PF_B8G8R8A8)
    {
        Converted.SetNumUninitialized(static_cast<int32>(Count));
        if (!ToBgra8(Frame.Format, Frame.Pixels.GetData(), Count, Converted.GetData())) { return false; }
        Colors = Converted.GetData();
    }
    OutWidth = Frame.Width; OutHeight = Frame.Height;
    const int32 Longest = FMath::Max(Frame.Width, Frame.Height);
    if (Longest > MaxDimension)
    {
        const double Scale = static_cast<double>(MaxDimension) / Longest;
        OutWidth = FMath::Max(1, FMath::RoundToInt(Frame.Width * Scale));
        OutHeight = FMath::Max(1, FMath::RoundToInt(Frame.Height * Scale));
        TArray<FColor> Scaled;
        Scaled.SetNumUninitialized(OutWidth * OutHeight);
        FImageUtils::ImageResize(Frame.Width, Frame.Height, TArrayView<const FColor>(Colors, static_cast<int32>(Count)),
            OutWidth, OutHeight, TArrayView<FColor>(Scaled.GetData(), Scaled.Num()), false, true);
        Converted = MoveTemp(Scaled);
        Colors = Converted.GetData();
    }
    // The JPEG wrapper reads quality 1 as "uncompressed" (100), so the lowest usable value is 2.
    return ImageWrapper->CompressImage(OutJpeg, EImageFormat::JPEG, FImageView(Colors, OutWidth, OutHeight, EGammaSpace::sRGB), FMath::Max(JpegQuality, 2))
        && !OutJpeg.IsEmpty();
}
