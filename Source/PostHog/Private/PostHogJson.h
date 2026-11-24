#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace PostHogJson
{
    bool IsBounded(const FString& Json, int32 MaxBytes = 1024 * 1024, int32 MaxDepth = 64);
    FString Encode(const TSharedRef<FJsonObject>& Object);
    FString EncodeValue(const TSharedPtr<FJsonValue>& Value);
    TSharedPtr<FJsonObject> Decode(const FString& Json, int32 MaxBytes = 1024 * 1024);
    TSharedPtr<FJsonValue> DecodeValue(const FString& Json, int32 MaxBytes = 1024 * 1024);
    void Merge(FJsonObject& Into, const FJsonObject& From);
}
