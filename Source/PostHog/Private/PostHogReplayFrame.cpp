#include "PostHogReplayFrame.h"

EPostHogResult UPostHogReplayFrame::Submit(const TArray<uint8>& Pixels)
{
    if (!IsInGameThread()) { return EPostHogResult::InvalidInput; }
    if (!Lease) { return EPostHogResult::Closed; }
    auto Frame = MoveTemp(Lease);
    return Frame->Submit(Pixels.GetData(), Pixels.Num());
}
EPostHogResult UPostHogReplayFrame::Abandon()
{
    if (!IsInGameThread()) { return EPostHogResult::InvalidInput; }
    if (!Lease) { return EPostHogResult::Closed; }
    auto Frame = MoveTemp(Lease); return Frame->Abandon();
}
int64 UPostHogReplayFrame::GetRequiredBytes() const { return IsInGameThread() && Lease ? Lease->GetRequiredBytes() : 0; }
void UPostHogReplayFrame::BeginDestroy() { Lease.Reset(); Super::BeginDestroy(); }
