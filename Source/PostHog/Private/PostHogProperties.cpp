#include "PostHogProperties.h"
#include "PostHogJson.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"

namespace PostHogJson
{
bool IsBounded(const FString& Json, int32 MaxBytes, int32 MaxDepth)
{
    if (Json.Len() > MaxBytes || FTCHARToUTF8(*Json).Length() > MaxBytes) { return false; }
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
        else if (Character == TEXT('{') || Character == TEXT('[')) { if (++Depth > MaxDepth) { return false; } }
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

TSharedPtr<FJsonObject> Decode(const FString& Json, int32 MaxBytes)
{
    TSharedPtr<FJsonObject> Object;
    if (!IsBounded(Json, MaxBytes) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), Object, FJsonSerializer::EFlags::StoreNumbersAsStrings)) { return nullptr; }
    return Object;
}

void Merge(FJsonObject& Into, const FJsonObject& From)
{
    for (const auto& Pair : From.Values) { Into.SetField(Pair.Key, Pair.Value); }
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
bool FPostHogValue::TryParse(const FString& Json, FPostHogValue& OutValue, int32 MaxBytes)
{
    if (!PostHogJson::IsBounded(Json, MaxBytes)) { return false; }
    // An array envelope allows the engine parser to accept JSON scalar roots.
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Values, FJsonSerializer::EFlags::StoreNumbersAsStrings) || Values.Num() != 1) { return false; }
    OutValue.Json = PostHogJson::EncodeValue(Values[0]);
    return true;
}
TSharedPtr<FJsonValue> FPostHogValue::ToNative() const
{
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Values, FJsonSerializer::EFlags::StoreNumbersAsStrings) || Values.Num() != 1) { return nullptr; }
    return Values[0];
}
bool FPostHogProperties::TryParse(const FString& Json, FPostHogProperties& OutProperties, int32 MaxBytes)
{
    auto Object = PostHogJson::Decode(Json, MaxBytes);
    if (!Object.IsValid()) { return false; }
    const FString Candidate = PostHogJson::Encode(Object.ToSharedRef());
    if (!PostHogJson::IsBounded(Candidate, MaxBytes)) { return false; }
    OutProperties.Json = Candidate; OutProperties.ByteLimit = MaxBytes;
    return true;
}
FPostHogProperties FPostHogProperties::FromNative(const TSharedRef<FJsonObject>& Object, int32 MaxBytes)
{
    FPostHogProperties Result;
    if (!TryParse(PostHogJson::Encode(Object), Result, MaxBytes)) { Result.Json.Empty(); }
    return Result;
}
bool FPostHogProperties::Set(const FString& Key, const FPostHogValue& Value)
{
    if (!Value.IsValid()) { return false; }
    auto Object = ToNative();
    Object->SetField(Key, Value.ToNative());
    const FString Candidate = PostHogJson::Encode(Object);
    if (!PostHogJson::IsBounded(Candidate, ByteLimit)) { return false; }
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
    auto Object = PostHogJson::Decode(Json, ByteLimit);
    return Object.IsValid() ? Object.ToSharedRef() : MakeShared<FJsonObject>();
}
