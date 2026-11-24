#pragma once
#include "CoreMinimal.h"
#include "PostHogClient.h"

struct PostHogReplayFrame;

/** Single-use replay frame reservation owned by `FPostHogReplayRecorder`. */
class FPostHogReplayFrame final
{
public:
    ~FPostHogReplayFrame();
    FPostHogReplayFrame(const FPostHogReplayFrame&) = delete;
    FPostHogReplayFrame& operator=(const FPostHogReplayFrame&) = delete;

    bool Submit(const uint8* Pixels, int64 NumBytes);
    void Abandon();

private:
    friend class FPostHogClient;
    FPostHogReplayFrame(PostHogReplayFrame* InFrame, TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe> InOwner)
        : Frame(InFrame), Owner(MoveTemp(InOwner)) {}

    PostHogReplayFrame* Frame = nullptr;
    TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe> Owner;
};
