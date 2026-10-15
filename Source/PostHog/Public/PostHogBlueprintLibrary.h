#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PostHogProperties.h"
#include "PostHogBlueprintLibrary.generated.h"

/** Typed property construction and value inspection for PostHog Blueprint nodes. */
UCLASS()
class POSTHOG_API UPostHogBlueprintLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeStringValue(const FString& Value);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeBooleanValue(bool Value);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeNumberValue(double Value);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeObjectValue(const FPostHogProperties& Value);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeArrayValue(const TArray<FPostHogValue>& Values);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FPostHogValue MakeNullValue();

    /** Mutates the supplied properties. Invalid/oversized values leave them unchanged. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Properties")
    static bool SetProperty(UPARAM(ref) FPostHogProperties& Properties, const FString& Key, const FPostHogValue& Value);
    /** Requires a valid JSON object. On failure the output is empty. */
    UFUNCTION(BlueprintCallable, Category = "PostHog|Properties")
    static bool ParseProperties(const FString& Json, FPostHogProperties& Properties);
    UFUNCTION(BlueprintPure, Category = "PostHog|Properties")
    static FString PropertiesToJson(const FPostHogProperties& Properties);

    /** True only for a boolean value, including false. No string/number coercion. */
    UFUNCTION(BlueprintPure, Category = "PostHog|Values")
    static bool TryGetBoolean(const FPostHogValue& Value, bool& Boolean);
    /** True only for a string value (such as a feature-flag variant). */
    UFUNCTION(BlueprintPure, Category = "PostHog|Values")
    static bool TryGetString(const FPostHogValue& Value, FString& String);
    /** Serializes any value/payload, preserving JSON type and explicit null. */
    UFUNCTION(BlueprintPure, Category = "PostHog|Values")
    static FString ValueToJson(const FPostHogValue& Value);
};
