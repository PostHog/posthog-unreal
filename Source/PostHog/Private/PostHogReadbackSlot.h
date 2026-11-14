#pragma once
#include "CoreMinimal.h"
#include "PixelFormat.h"

/** A readback's staging allocation is valid only for its original crop extent/format.
 * The caller reports outstanding GPU ownership until its job/fence retires. */
template <typename ReadbackType>
class TPostHogReadbackSlot final
{
public:
    bool Prepare(FIntPoint Extent, EPixelFormat Format, bool bCopyOutstanding,
        TFunctionRef<TUniquePtr<ReadbackType>()> Create)
    {
        if (bCopyOutstanding) { return false; }
        if (!Readback || Extent != AllocatedExtent || Format != AllocatedFormat)
        {
            auto Replacement = Create();
            if (!Replacement) { return false; }
            Readback = MoveTemp(Replacement); AllocatedExtent = Extent; AllocatedFormat = Format;
        }
        return true;
    }
    ReadbackType* operator->() const { return Readback.Get(); }
    ReadbackType* Get() const { return Readback.Get(); }
    void Reset()
    { Readback.Reset(); AllocatedExtent = FIntPoint::ZeroValue; AllocatedFormat = PF_Unknown; }
private:
    TUniquePtr<ReadbackType> Readback;
    FIntPoint AllocatedExtent = FIntPoint::ZeroValue;
    EPixelFormat AllocatedFormat = PF_Unknown;
};
