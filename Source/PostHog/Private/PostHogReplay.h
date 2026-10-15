#pragma once

#include "CoreMinimal.h"
#include "PixelFormat.h"
#include "PostHogClient.h"
#include <atomic>

class FEvent;
class FRunnable;
class FRunnableThread;
class FRHIGPUTextureReadback;
class ISlateViewportProvider;
class IImageWrapperModule;
class SWindow;

/** Screenshot session replay: captures the main game window's back buffer on the render
 * thread without stalling, then converts, scales, and JPEG-encodes frames on one worker.
 * Start, SetPaused, and Stop run on the game thread. */
class FPostHogReplayRecorder final
{
public:
    using FPtr = TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe>;
    /** Null when this process cannot render or Slate is unavailable. */
    static FPtr Start(FPostHogClient::FPtr Client, const FPostHogReplaySettings& Settings);
    /** Module teardown: stops every live recorder before clients close. */
    static void StopAll();

    void SetPaused(bool bInPaused) { bPaused.store(bInPaused); }
    /** Idempotent. Removes the render hook, joins the worker, and releases GPU readbacks. */
    void Stop();
    ~FPostHogReplayRecorder();

private:
    struct FSlot;
    struct FWorker;
    struct FFrame
    {
        TArray64<uint8> Pixels;
        EPixelFormat Format = PF_Unknown;
        int32 Width = 0;
        int32 Height = 0;
        FDateTime CapturedAt;
    };

    FPostHogReplayRecorder(FPostHogClient::FPtr InClient, const FPostHogReplaySettings& Settings);
    void RunWorker();
    void OnBackBufferReady(SWindow& Window, ISlateViewportProvider& Provider);
    void DrainReadySlots();
    bool Encode(const FFrame& Frame, TArray64<uint8>& OutJpeg, int32& OutWidth, int32& OutHeight) const;
    void Submit(const FFrame& Frame);
    /** Caller holds QueueMutex. */
    void RecycleLocked(TArray64<uint8>&& Pixels);
    void RefreshGate();

    FPostHogClient::FPtr Client;
    IImageWrapperModule* ImageWrapper = nullptr;
    const double IntervalSeconds;
    const int32 MaxDimension;
    const int32 JpegQuality;
    FDelegateHandle BackBufferHandle;
    TUniquePtr<FRunnable> Worker;
    FRunnableThread* Thread = nullptr;
    FEvent* Wake = nullptr;
    std::atomic<bool> bStopping{false};
    std::atomic<bool> bPaused{false};
    std::atomic<bool> bGateOpen{false};
    std::atomic<bool> bWarnedFormat{false};
    /** Identity only, never dereferenced off the game thread. */
    std::atomic<const SWindow*> TargetWindow{nullptr};

    // Readback ring and capture throttle, owned by the render thread.
    TArray<TUniquePtr<FSlot>> Slots;
    FIntPoint RingSize = FIntPoint::ZeroValue;
    EPixelFormat RingFormat = PF_Unknown;
    double LastCaptureSeconds = -1.0e9;

    FCriticalSection QueueMutex;
    TArray<FFrame> Queue;
    TArray<TArray64<uint8>> FreeBuffers;

    std::atomic<int64> Captured{0}, DroppedNoSlot{0}, DroppedBacklog{0}, DroppedGated{0};
    std::atomic<int64> RenderSamples{0}, RenderNanoseconds{0};
    // Worker only; read by Stop after the join.
    double WorkerSeconds = 0.0;
    int64 WorkerSamples = 0;
    int64 LastJpegBytes = 0;
    int64 Results[5] = {};
};
