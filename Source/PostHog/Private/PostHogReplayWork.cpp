#include "PostHogReplayWork.h"
#include "Misc/ScopeLock.h"

TUniquePtr<FPostHogReplayWork> FPostHogReplayWorkState::Begin(FPostHogClient& Client, int32 Width, int32 Height)
{
    FScopeLock Lock(&Mutex);
    if (bStopped || Count.load() >= Capacity || Width <= 0 || Width > MAX_int32 / 4) { return nullptr; }
    TUniquePtr<FPostHogReplayFrame> Frame;
    if (Client.BeginReplayFrame(Width, Height, Width * 4, Frame) != EPostHogResult::Ok) { return nullptr; }
    Count.fetch_add(1);
    return TUniquePtr<FPostHogReplayWork>(new FPostHogReplayWork(AsShared(), Revision, MoveTemp(Frame)));
}
bool FPostHogReplayWorkState::BeginCopy(FPostHogClient& Client, int32 Width, int32 Height,
    TFunctionRef<void(TUniquePtr<FPostHogReplayWork>)> Copy)
{
    FScopeLock Admission(&AdmissionMutex);
    auto Work = Begin(Client, Width, Height);
    if (!Work) { return false; }
    Copy(MoveTemp(Work)); return true;
}
void FPostHogReplayWorkState::Cancel()
{ FScopeLock Admission(&AdmissionMutex); FScopeLock Lock(&Mutex); ++Revision; }
void FPostHogReplayWorkState::Stop()
{ FScopeLock Admission(&AdmissionMutex); FScopeLock Lock(&Mutex); bStopped = true; ++Revision; }
FPostHogReplayWork::~FPostHogReplayWork()
{ FScopeLock Lock(&State->Mutex); Frame.Reset(); State->Count.fetch_sub(1); }
bool FPostHogReplayWork::IsCurrent() const
{ FScopeLock Lock(&State->Mutex); return Frame && !State->bStopped && Revision == State->Revision; }
EPostHogResult FPostHogReplayWork::Submit(const TArray64<uint8>& Pixels)
{
    FScopeLock Lock(&State->Mutex);
    if (!Frame) { return EPostHogResult::Closed; }
    if (State->bStopped || Revision != State->Revision) { Frame.Reset(); return EPostHogResult::Inactive; }
    const auto Result = Frame->Submit(Pixels.GetData(), Pixels.Num()); Frame.Reset(); return Result;
}
