#pragma once
#include "CoreMinimal.h"
#include "PostHogClient.h"

class UGameInstance;

/** Captures the owning game viewport's backbuffer into Rust replay frames. Game-thread owned. */
class FPostHogReplayRecorder final
{
public:
    using FPtr = TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe>;
    static FPtr Attach(UGameInstance* Instance, FPostHogClient::FPtr Client, const FPostHogReplaySettings& Settings);
    ~FPostHogReplayRecorder();

    void SetAvailable(bool bAvailable);
    void Stop();

private:
    FPostHogReplayRecorder();
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
