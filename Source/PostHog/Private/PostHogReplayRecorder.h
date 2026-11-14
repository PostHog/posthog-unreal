#pragma once
#include "CoreMinimal.h"
#include "PostHogClient.h"

class UGameInstance;
class FPostHogReplayRecorder final : public TSharedFromThis<FPostHogReplayRecorder, ESPMode::ThreadSafe>
{
public:
    using FPtr = TSharedPtr<FPostHogReplayRecorder, ESPMode::ThreadSafe>;
    static FPtr Attach(UGameInstance* Instance, FPostHogClient::FPtr Client, const FPostHogReplaySettings& Settings);
    static void StopAll();
    /** Safe on ordinary host workers as well as engine rendering tasks. */
    static bool ShouldDeferRetirement(const TSharedFuture<bool>& Done);
    void SetAvailable(bool bAvailable);
    /** Game-thread owner teardown; native close is still the client's one public operation. */
    void Stop();
    ~FPostHogReplayRecorder();
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
    FPostHogReplayRecorder();
    void Cancel();
    TPair<TSharedFuture<bool>, bool> Retire();
};
