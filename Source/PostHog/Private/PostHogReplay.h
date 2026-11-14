#pragma once
#include "CoreMinimal.h"
#include "PixelFormat.h"

/** Pixel preparation for engine readbacks. This does not acquire pixels or apply privacy masks. */
namespace PostHogReplay
{
    bool MatchesSource(const void* PaintedWindow, const void* Window, FIntPoint PaintedPixels, FIntPoint Pixels);
    bool SupportedFormat(EPixelFormat Format);
    FIntPoint ScaledSize(int32 Width, int32 Height, int32 MaxDimension);
    /** Inward-rounded, axis-aligned viewport bounds in its own window's pixel space. */
    bool ViewportRect(const FVector2D& TopLeft, const FVector2D& TopRight, const FVector2D& BottomRight,
        const FVector2D& WindowLocalSize, FIntPoint WindowPixels, FIntRect& Out);
    bool CopyRows(const uint8* Source, int32 PitchPixels, int32 BufferHeight, EPixelFormat Format,
        int32 Width, int32 Height, TArray64<uint8>& Out);
    bool PrepareBgra(EPixelFormat Format, const TArray64<uint8>& Source, int32 Width, int32 Height,
        int32 MaxDimension, TArray64<uint8>& Pixels, int32& OutWidth, int32& OutHeight);
}
