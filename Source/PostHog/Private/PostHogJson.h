#pragma once
#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"

namespace PostHogJson
{
    inline const TCHAR* const LibName = TEXT("posthog-unreal");
    inline const TCHAR* const LibVersion = TEXT("0.0.1");

    bool IsBounded(const FString& Json);
    FString Encode(const TSharedRef<FJsonObject>& Object);
    FString EncodeValue(const TSharedPtr<FJsonValue>& Value);
    TSharedPtr<FJsonObject> Decode(const FString& Json);
    void Merge(FJsonObject& Into, const FJsonObject& From);
    void RemoveNullObjectProperties(const TSharedPtr<FJsonValue>& Value);
    int64 UnixMilliseconds(const FDateTime& Time);
    FString NewUuidV7(const FDateTime& Now);
}
