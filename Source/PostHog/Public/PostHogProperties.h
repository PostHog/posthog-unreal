#pragma once

#include "CoreMinimal.h"
#include "PostHogProperties.generated.h"

class FJsonObject;
class FJsonValue;
struct FPostHogProperties;

/** JSON value with value-copy semantics, including explicit JSON null. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogValue
{
    GENERATED_BODY()

    static FPostHogValue String(const FString& Value);
    static FPostHogValue Boolean(bool Value);
    /** Non-finite numbers become JSON null. */
    static FPostHogValue Number(double Value);
    static FPostHogValue Object(const FPostHogProperties& Value);
    static FPostHogValue Array(const TArray<FPostHogValue>& Values);
    static bool TryParse(const FString& Json, FPostHogValue& OutValue);

    bool IsValid() const { return !Json.IsEmpty(); }
    FString ToJson() const { return Json; }
    TSharedPtr<FJsonValue> ToNative() const;

private:
    UPROPERTY()
    FString Json = TEXT("null");
};

/** Typed event properties. Copies never share mutable JSON objects. */
USTRUCT(BlueprintType)
struct POSTHOG_API FPostHogProperties
{
    GENERATED_BODY()

    static bool TryParse(const FString& Json, FPostHogProperties& OutProperties);
    static FPostHogProperties FromNative(const TSharedRef<FJsonObject>& Object);

    bool Set(const FString& Key, const FPostHogValue& Value);
    bool SetString(const FString& Key, const FString& Value);
    bool SetBoolean(const FString& Key, bool Value);
    bool SetNumber(const FString& Key, double Value);
    void Remove(const FString& Key);
    bool IsEmpty() const;
    bool IsValid() const { return !Json.IsEmpty(); }
    FString ToJson() const { return Json; }
    TSharedRef<FJsonObject> ToNative() const;

private:
    UPROPERTY()
    FString Json = TEXT("{}");
};
