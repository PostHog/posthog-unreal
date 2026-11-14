#pragma once
#include "CoreMinimal.h"
#include "PostHogClient.h"
#include <atomic>

class FPostHogReplayWork;

/** Bounded host staging and engine-source cancellation only. Native leases own SDK admission. */
class FPostHogReplayWorkState final : public TSharedFromThis<FPostHogReplayWorkState, ESPMode::ThreadSafe>
{
public:
    static constexpr int32 Capacity = 2;
    TUniquePtr<FPostHogReplayWork> Begin(FPostHogClient& Client, int32 Width, int32 Height);
    bool BeginCopy(FPostHogClient& Client, int32 Width, int32 Height,
        TFunctionRef<void(TUniquePtr<FPostHogReplayWork>)> Copy);
    void Cancel();
    void Stop();
    int32 Pending() const { return Count.load(); }
private:
    friend class FPostHogReplayWork;
    FCriticalSection AdmissionMutex;
    FCriticalSection Mutex;
    uint64 Revision = 0;
    bool bStopped = false;
    std::atomic<int32> Count{0};
};

/** One host permit and its exact native reservation, moved through GPU and CPU stages. */
class FPostHogReplayWork final
{
public:
    ~FPostHogReplayWork();
    FPostHogReplayWork(const FPostHogReplayWork&) = delete;
    bool IsCurrent() const;
    EPostHogResult Submit(const TArray64<uint8>& Pixels);
private:
    friend class FPostHogReplayWorkState;
    FPostHogReplayWork(TSharedRef<FPostHogReplayWorkState, ESPMode::ThreadSafe> InState, uint64 InRevision,
        TUniquePtr<FPostHogReplayFrame>&& InFrame) : State(InState), Revision(InRevision), Frame(MoveTemp(InFrame)) {}
    TSharedRef<FPostHogReplayWorkState, ESPMode::ThreadSafe> State;
    uint64 Revision;
    TUniquePtr<FPostHogReplayFrame> Frame;
};
