#include "PostHogBlueprintLibrary.h"
#include "Dom/JsonValue.h"

FPostHogValue UPostHogBlueprintLibrary::MakeStringValue(const FString& Value) { return FPostHogValue::String(Value); }
FPostHogValue UPostHogBlueprintLibrary::MakeBooleanValue(bool Value) { return FPostHogValue::Boolean(Value); }
FPostHogValue UPostHogBlueprintLibrary::MakeNumberValue(double Value) { return FPostHogValue::Number(Value); }
FPostHogValue UPostHogBlueprintLibrary::MakeObjectValue(const FPostHogProperties& Value) { return FPostHogValue::Object(Value); }
FPostHogValue UPostHogBlueprintLibrary::MakeArrayValue(const TArray<FPostHogValue>& Values) { return FPostHogValue::Array(Values); }
FPostHogValue UPostHogBlueprintLibrary::MakeNullValue() { return {}; }

bool UPostHogBlueprintLibrary::SetProperty(FPostHogProperties& Properties, const FString& Key, const FPostHogValue& Value)
{ return Properties.Set(Key, Value); }
bool UPostHogBlueprintLibrary::ParseProperties(const FString& Json, FPostHogProperties& Properties)
{ Properties = {}; return FPostHogProperties::TryParse(Json, Properties); }
FString UPostHogBlueprintLibrary::PropertiesToJson(const FPostHogProperties& Properties) { return Properties.ToJson(); }

bool UPostHogBlueprintLibrary::TryGetBoolean(const FPostHogValue& Value, bool& Boolean)
{
    Boolean = false;
    const auto Native = Value.ToNative();
    return Native && Native->Type == EJson::Boolean && Native->TryGetBool(Boolean);
}
bool UPostHogBlueprintLibrary::TryGetString(const FPostHogValue& Value, FString& String)
{
    String.Empty();
    const auto Native = Value.ToNative();
    return Native && Native->Type == EJson::String && Native->TryGetString(String);
}
FString UPostHogBlueprintLibrary::ValueToJson(const FPostHogValue& Value) { return Value.ToJson(); }
