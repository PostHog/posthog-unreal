#include "PostHogReplay.h"
#include "ImageUtils.h"
#include "Math/Float16Color.h"

bool PostHogReplay::MatchesSource(const void* PaintedWindow, const void* Window, FIntPoint PaintedPixels, FIntPoint Pixels)
{ return PaintedWindow && PaintedWindow == Window && PaintedPixels == Pixels && Pixels.X > 0 && Pixels.Y > 0; }
bool PostHogReplay::SupportedFormat(EPixelFormat Format)
{ return Format == PF_B8G8R8A8 || Format == PF_R8G8B8A8 || Format == PF_A2B10G10R10 || Format == PF_FloatRGBA; }
FIntPoint PostHogReplay::ScaledSize(int32 Width, int32 Height, int32 MaxDimension)
{
    if (Width <= 0 || Height <= 0 || MaxDimension <= 0 || static_cast<int64>(Width) * Height > MAX_int32) { return FIntPoint::ZeroValue; }
    const double Scale = FMath::Min(1.0, static_cast<double>(MaxDimension) / FMath::Max(Width, Height));
    return FIntPoint(FMath::Max(1, FMath::RoundToInt(Width * Scale)), FMath::Max(1, FMath::RoundToInt(Height * Scale)));
}
bool PostHogReplay::ViewportRect(const FVector2D& A, const FVector2D& B, const FVector2D& C,
    const FVector2D& Size, FIntPoint Pixels, FIntRect& Out)
{
    Out = FIntRect();
    if (A.ContainsNaN() || B.ContainsNaN() || C.ContainsNaN() || Size.ContainsNaN() || Size.X <= 0 || Size.Y <= 0 || Pixels.X <= 0 || Pixels.Y <= 0 ||
        !FMath::IsNearlyEqual(A.Y, B.Y, 0.01) || !FMath::IsNearlyEqual(B.X, C.X, 0.01) || B.X <= A.X || C.Y <= A.Y ||
        A.X < 0 || A.Y < 0 || C.X > Size.X || C.Y > Size.Y) { return false; }
    Out = FIntRect(FMath::CeilToInt((A.X / Size.X) * Pixels.X), FMath::CeilToInt((A.Y / Size.Y) * Pixels.Y),
        FMath::FloorToInt((C.X / Size.X) * Pixels.X), FMath::FloorToInt((C.Y / Size.Y) * Pixels.Y));
    return Out.Width() > 0 && Out.Height() > 0;
}
bool PostHogReplay::CopyRows(const uint8* Source, int32 Pitch, int32 BufferHeight, EPixelFormat Format,
    int32 Width, int32 Height, TArray64<uint8>& Out)
{
    Out.Empty();
    if (!Source || !SupportedFormat(Format) || Width <= 0 || Height <= 0 || Pitch < Width ||
        (BufferHeight != 0 && BufferHeight < Height) || static_cast<int64>(Width) * Height > MAX_int32) { return false; }
    const int64 Bpp = Format == PF_FloatRGBA ? 8 : 4;
    if (static_cast<int64>(Pitch) > MAX_int64 / Bpp / Height) { return false; }
    const int64 RowBytes = static_cast<int64>(Width) * Bpp;
    Out.SetNumUninitialized(RowBytes * Height);
    for (int32 Row = 0; Row < Height; ++Row)
    { FMemory::Memcpy(Out.GetData() + Row * RowBytes, Source + static_cast<int64>(Row) * Pitch * Bpp, RowBytes); }
    return true;
}

bool PostHogReplay::PrepareBgra(EPixelFormat Format, const TArray64<uint8>& Source, int32 Width, int32 Height,
    int32 MaxDimension, TArray64<uint8>& Pixels, int32& OutWidth, int32& OutHeight)
{
    Pixels.Empty(); OutWidth = 0; OutHeight = 0;
    const int64 Count = static_cast<int64>(Width) * Height;
    const FIntPoint Target = ScaledSize(Width, Height, MaxDimension);
    if (Target == FIntPoint::ZeroValue || !SupportedFormat(Format)) { return false; }
    const int32 BytesPerPixel = Format == PF_FloatRGBA ? sizeof(FFloat16Color) : 4;
    if (Source.Num() < Count * BytesPerPixel) { return false; }
    TArray<FColor> Colors; Colors.SetNumUninitialized(static_cast<int32>(Count));
    const uint8* Input = Source.GetData();
    for (int32 Index = 0; Index < Count; ++Index, Input += BytesPerPixel)
    {
        switch (Format)
        {
        case PF_B8G8R8A8: Colors[Index] = FColor(Input[2], Input[1], Input[0]); break;
        case PF_R8G8B8A8: Colors[Index] = FColor(Input[0], Input[1], Input[2]); break;
        case PF_A2B10G10R10:
        {
            uint32 Packed; FMemory::Memcpy(&Packed, Input, sizeof(Packed));
            Colors[Index] = FColor::MakeRequantizeFrom1010102(Packed & 0x3FF, (Packed >> 10) & 0x3FF, (Packed >> 20) & 0x3FF, 3);
            break;
        }
        case PF_FloatRGBA:
        {
            FFloat16Color Half; FMemory::Memcpy(&Half, Input, sizeof(Half)); Colors[Index] = FLinearColor(Half).ToFColorSRGB(); break;
        }
        default: return false;
        }
        Colors[Index].A = 255;
    }
    OutWidth = Target.X; OutHeight = Target.Y;
    if (OutWidth != Width || OutHeight != Height)
    {
        TArray<FColor> Scaled; Scaled.SetNumUninitialized(OutWidth * OutHeight);
        FImageUtils::ImageResize(Width, Height, TArrayView<const FColor>(Colors), OutWidth, OutHeight, TArrayView<FColor>(Scaled), false, true);
        Colors = MoveTemp(Scaled);
    }
    Pixels.Append(reinterpret_cast<const uint8*>(Colors.GetData()), static_cast<int64>(Colors.Num()) * sizeof(FColor));
    return true;
}
