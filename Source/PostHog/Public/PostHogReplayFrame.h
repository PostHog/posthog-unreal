#pragma once
#include "CoreMinimal.h"
#include "PostHogClient.h"
#include "UObject/Object.h"
#include "PostHogReplayFrame.generated.h"

/** Blueprint-owned single-use screenshot acquisition. GC abandons an unsubmitted lease. */
UCLASS(BlueprintType, NotBlueprintable)
class POSTHOG_API UPostHogReplayFrame : public UObject
{
    GENERATED_BODY()
public:
    /** Complete composited/downscaled/masked top-down BGRA8888; alpha is ignored.
     * Every submission consumes the lease, including invalid pixels and stale context. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    EPostHogResult Submit(const TArray<uint8>& Pixels);
    UFUNCTION(BlueprintCallable, Category = "PostHog|Session Replay")
    EPostHogResult Abandon();
    UFUNCTION(BlueprintPure, Category = "PostHog|Session Replay")
    int64 GetRequiredBytes() const;
    virtual void BeginDestroy() override;
private:
    friend class UPostHogSubsystem;
    TUniquePtr<FPostHogReplayFrame> Lease;
};
