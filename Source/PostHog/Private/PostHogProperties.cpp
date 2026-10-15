#include "PostHogProperties.h"
#include "PostHogJson.h"
#include "Serialization/JsonSerializer.h"

namespace PostHogJson
{
bool IsBounded(const FString& Json)
{
    if (Json.Len() > 1024 * 1024 || FTCHARToUTF8(*Json).Length() > 1024 * 1024) { return false; }
    int32 Depth = 0;
    bool bString = false;
    bool bEscape = false;
    for (TCHAR Character : Json)
    {
        if (bString)
        {
            if (bEscape) { bEscape = false; }
            else if (Character == TEXT('\\')) { bEscape = true; }
            else if (Character == TEXT('"')) { bString = false; }
        }
        else if (Character == TEXT('"')) { bString = true; }
        else if (Character == TEXT('{') || Character == TEXT('[')) { if (++Depth > 64) { return false; } }
        else if (Character == TEXT('}') || Character == TEXT(']')) { if (--Depth < 0) { return false; } }
    }
    return Depth == 0 && !bString;
}

FString Encode(const TSharedRef<FJsonObject>& Object)
{
    FString Json;
    FJsonSerializer::Serialize(Object, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}

FString EncodeValue(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) { return FString(); }
    FString Json;
    FJsonSerializer::Serialize(Value.ToSharedRef(), TEXT(""), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Json));
    return Json;
}

TSharedPtr<FJsonObject> Decode(const FString& Json)
{
    TSharedPtr<FJsonObject> Object;
    if (!IsBounded(Json) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object)) { return nullptr; }
    return Object;
}

void Merge(FJsonObject& Into, const FJsonObject& From)
{
    for (const auto& Pair : From.Values) { Into.SetField(Pair.Key, Pair.Value); }
}

void RemoveNullObjectProperties(const TSharedPtr<FJsonValue>& Value)
{
    if (!Value.IsValid()) { return; }
    if (Value->Type == EJson::Object)
    {
        auto Object = Value->AsObject();
        for (auto It = Object->Values.CreateIterator(); It; ++It)
        {
            if (!It.Value().IsValid() || It.Value()->IsNull()) { It.RemoveCurrent(); }
            else { RemoveNullObjectProperties(It.Value()); }
        }
    }
    else if (Value->Type == EJson::Array)
    {
        for (const auto& Item : Value->AsArray()) { RemoveNullObjectProperties(Item); }
    }
}

int64 UnixMilliseconds(const FDateTime& Time)
{
    return (Time - FDateTime(1970, 1, 1)).GetTicks() / ETimespan::TicksPerMillisecond;
}

FString NewUuidV7(const FDateTime& Now)
{
    // RFC 9562: 48-bit Unix milliseconds, version 7, variant 10, random remainder.
    const uint64 Milliseconds = static_cast<uint64>(UnixMilliseconds(Now));
    FGuid Random = FGuid::NewGuid();
    return FGuid(static_cast<uint32>(Milliseconds >> 16),
        (static_cast<uint32>(Milliseconds & 0xffff) << 16) | 0x7000 | (Random.B & 0x0fff),
        0x80000000 | (Random.C & 0x3fffffff), Random.D).ToString(EGuidFormats::DigitsWithHyphensLower);
}
}

FPostHogValue FPostHogValue::String(const FString& Value)
{
    FPostHogValue Result;
    Result.Json = PostHogJson::EncodeValue(MakeShared<FJsonValueString>(Value));
    return Result;
}
FPostHogValue FPostHogValue::Boolean(bool Value)
{
    FPostHogValue Result;
    Result.Json = Value ? TEXT("true") : TEXT("false");
    return Result;
}
FPostHogValue FPostHogValue::Number(double Value)
{
    FPostHogValue Result;
    Result.Json = FMath::IsFinite(Value) ? PostHogJson::EncodeValue(MakeShared<FJsonValueNumber>(Value)) : FString();
    return Result;
}
FPostHogValue FPostHogValue::Object(const FPostHogProperties& Value)
{
    FPostHogValue Result;
    Result.Json = Value.ToJson();
    return Result;
}
FPostHogValue FPostHogValue::Array(const TArray<FPostHogValue>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Array;
    for (const auto& Value : Values)
    {
        if (!Value.IsValid()) { FPostHogValue Invalid; Invalid.Json.Empty(); return Invalid; }
        Array.Add(Value.ToNative());
    }
    FPostHogValue Result;
    Result.Json = PostHogJson::EncodeValue(MakeShared<FJsonValueArray>(Array));
    if (!PostHogJson::IsBounded(Result.Json)) { Result.Json.Empty(); }
    return Result;
}
bool FPostHogValue::TryParse(const FString& Json, FPostHogValue& OutValue)
{
    if (!PostHogJson::IsBounded(Json)) { return false; }
    // An array envelope allows the engine parser to accept JSON scalar roots.
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Values) || Values.Num() != 1) { return false; }
    OutValue.Json = PostHogJson::EncodeValue(Values[0]);
    return true;
}
TSharedPtr<FJsonValue> FPostHogValue::ToNative() const
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Values) || Values.Num() != 1) { return nullptr; }
    return Values[0];
}
bool FPostHogProperties::TryParse(const FString& Json, FPostHogProperties& OutProperties)
{
    auto Object = PostHogJson::Decode(Json);
    if (!Object.IsValid()) { return false; }
    OutProperties.Json = PostHogJson::Encode(Object.ToSharedRef());
    return true;
}
FPostHogProperties FPostHogProperties::FromNative(const TSharedRef<FJsonObject>& Object)
{
    FPostHogProperties Result;
    if (!TryParse(PostHogJson::Encode(Object), Result)) { Result.Json.Empty(); }
    return Result;
}
bool FPostHogProperties::Set(const FString& Key, const FPostHogValue& Value)
{
    if (!Value.IsValid()) { return false; }
    auto Object = ToNative();
    Object->SetField(Key, Value.ToNative());
    const FString Candidate = PostHogJson::Encode(Object);
    if (!PostHogJson::IsBounded(Candidate)) { return false; }
    Json = Candidate;
    return true;
}
bool FPostHogProperties::SetString(const FString& Key, const FString& Value) { return Set(Key, FPostHogValue::String(Value)); }
bool FPostHogProperties::SetBoolean(const FString& Key, bool Value) { return Set(Key, FPostHogValue::Boolean(Value)); }
bool FPostHogProperties::SetNumber(const FString& Key, double Value) { return Set(Key, FPostHogValue::Number(Value)); }
void FPostHogProperties::Remove(const FString& Key)
{
    auto Object = ToNative(); Object->RemoveField(Key); Json = PostHogJson::Encode(Object);
}
bool FPostHogProperties::IsEmpty() const { return ToNative()->Values.IsEmpty(); }
TSharedRef<FJsonObject> FPostHogProperties::ToNative() const
{
    auto Object = PostHogJson::Decode(Json);
    return Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>();
}
