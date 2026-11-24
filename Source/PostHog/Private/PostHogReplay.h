#pragma once
#include "CoreMinimal.h"
#include "PixelFormat.h"

/** Pure viewport rect math and pixel conversion/downscaling into top-down BGRA8888. */
namespace PostHogReplay
{
    bool MatchesSource(const void* PaintedWindow, const void* Window, FIntPoint PaintedPixels, FIntPoint Pixels);
    bool SupportedFormat(EPixelFormat Format);
    FIntPoint ScaledSize(int32 Width, int32 Height, int32 MaxDimension);
    bool ViewportRect(const FVector2D& A, const FVector2D& B, const FVector2D& C,
        const FVector2D& Size, FIntPoint Pixels, FIntRect& Out);
    bool CopyRows(const uint8* Source, int32 Pitch, int32 BufferHeight, EPixelFormat Format,
        int32 Width, int32 Height, TArray64<uint8>& Out);
    bool PrepareBgra(EPixelFormat Format, const TArray64<uint8>& Source, int32 Width, int32 Height,
        int32 MaxDimension, TArray64<uint8>& Pixels, int32& OutWidth, int32& OutHeight);
}
