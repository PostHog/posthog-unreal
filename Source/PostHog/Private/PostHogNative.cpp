#include "PostHogNative.h"
#include "PostHogJson.h"
#include "Serialization/JsonSerializer.h"

phu_bytes PostHogNative::View(const FTCHARToUTF8& Text)
{ return {reinterpret_cast<const uint8_t*>(Text.Get()), static_cast<size_t>(Text.Length())}; }

TSharedPtr<FJsonValue> PostHogNative::FBuffer::Read() const
{
    if (!Value.data || Value.len > PHU_MAX_OUTPUT) { return nullptr; }
    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Value.data), static_cast<int32>(Value.len));
    const FString Json(Text.Length(), Text.Get());
    if (!PostHogJson::IsBounded(Json, PHU_MAX_OUTPUT)) { return nullptr; }
    TArray<TSharedPtr<FJsonValue>> Values;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(TEXT("[") + Json + TEXT("]")), Values,
        FJsonSerializer::EFlags::StoreNumbersAsStrings) || Values.Num() != 1) { return nullptr; }
    return Values[0];
}

EPostHogResult PostHogNative::Result(phu_status Status)
{
    switch (Status)
    {
    case PHU_OK: return EPostHogResult::Ok;
    case PHU_ACCEPTED: return EPostHogResult::Accepted;
    case PHU_INVALID_INPUT: return EPostHogResult::InvalidInput;
    case PHU_CLOSED: return EPostHogResult::Closed;
    case PHU_UNSUPPORTED_ABI: return EPostHogResult::UnsupportedAbi;
    case PHU_DISABLED: return EPostHogResult::Disabled;
    case PHU_FULL: return EPostHogResult::Full;
    case PHU_OUTPUT_TOO_LARGE: return EPostHogResult::OutputTooLarge;
    case PHU_OPTED_OUT: return EPostHogResult::OptedOut;
    case PHU_DROPPED: return EPostHogResult::Dropped;
    case PHU_REENTRANT: return EPostHogResult::Reentrant;
    case PHU_DEFERRED: return EPostHogResult::Deferred;
    case PHU_INACTIVE: return EPostHogResult::Inactive;
    case PHU_WRONG_RECORDER: return EPostHogResult::WrongRecorder;
    case PHU_STALE: return EPostHogResult::Stale;
    default: return EPostHogResult::InternalError;
    }
}
TSharedRef<FJsonObject> PostHogNative::Operation(const TCHAR* Name)
{ auto Object = MakeShared<FJsonObject>(); Object->SetStringField(TEXT("op"), Name); return Object; }

FPostHogDrainResult PostHogNative::Drain(phu_status Status, const FBuffer& Buffer)
{
    FPostHogDrainResult Out; Out.Status = Result(Status);
    if (Status != PHU_OK) { return Out; }
    const auto Value = Buffer.Read();
    FString State, Delivery;
    if (!Value || Value->Type != EJson::Object || !Value->AsObject()->TryGetStringField(TEXT("state"), State) ||
        !Value->AsObject()->TryGetStringField(TEXT("delivery"), Delivery))
    { Out.Status = EPostHogResult::InvalidResponse; return Out; }
    const TMap<FString, EPostHogPersistenceOutcome> States = {
        {TEXT("persisted"), EPostHogPersistenceOutcome::Persisted}, {TEXT("memory_only"), EPostHogPersistenceOutcome::MemoryOnly},
        {TEXT("unavailable"), EPostHogPersistenceOutcome::Unavailable}, {TEXT("write_failed"), EPostHogPersistenceOutcome::WriteFailed},
        {TEXT("timed_out"), EPostHogPersistenceOutcome::TimedOut}, {TEXT("deferred"), EPostHogPersistenceOutcome::Deferred},
        {TEXT("disabled"), EPostHogPersistenceOutcome::Disabled}, {TEXT("not_requested"), EPostHogPersistenceOutcome::NotRequested}
    };
    const TMap<FString, EPostHogDeliveryOutcome> Deliveries = {
        {TEXT("attempted"), EPostHogDeliveryOutcome::Attempted}, {TEXT("stopped"), EPostHogDeliveryOutcome::Stopped},
        {TEXT("unavailable"), EPostHogDeliveryOutcome::Unavailable}, {TEXT("closed"), EPostHogDeliveryOutcome::Closed},
        {TEXT("deferred"), EPostHogDeliveryOutcome::Deferred}, {TEXT("disabled"), EPostHogDeliveryOutcome::Disabled}
    };
    const auto* FoundState = States.Find(State); const auto* FoundDelivery = Deliveries.Find(Delivery);
    Out.State = FoundState ? *FoundState : EPostHogPersistenceOutcome::Unknown;
    Out.Delivery = FoundDelivery ? *FoundDelivery : EPostHogDeliveryOutcome::Unknown;
    return Out;
}
FPostHogFeatureFlagResult PostHogNative::Flag(const TSharedPtr<FJsonValue>& Value)
{
    FPostHogFeatureFlagResult Out; Out.Status = EPostHogResult::Ok;
    if (Value && Value->IsNull()) { return Out; }
    if (!Value || Value->Type != EJson::Object) { Out.Status = EPostHogResult::InvalidResponse; return Out; }
    const auto Object = Value->AsObject();
    const auto Variant = Object->TryGetField(TEXT("variant"));
    const auto Payload = Object->TryGetField(TEXT("payload"));
    if (!Object->TryGetBoolField(TEXT("enabled"), Out.bEnabled) ||
        !Object->TryGetBoolField(TEXT("has_payload"), Out.bHasPayload) || !Variant || !Payload ||
        (Variant->Type != EJson::String && !Variant->IsNull()))
    { Out.Status = EPostHogResult::InvalidResponse; return Out; }
    Out.Value = Variant->Type == EJson::String ? FPostHogValue::String(Variant->AsString()) : FPostHogValue::Boolean(Out.bEnabled);
    Out.Record = FPostHogProperties::FromNative(Object.ToSharedRef(), PHU_MAX_OUTPUT);
    if (!Out.Record.IsValid() || !FPostHogValue::TryParse(PostHogJson::EncodeValue(Payload), Out.Payload, PHU_MAX_OUTPUT))
    { Out.Status = EPostHogResult::InvalidResponse; return Out; }
    Out.bFound = true;
    return Out;
}
FPostHogFeatureFlags PostHogNative::Flags(const TSharedPtr<FJsonValue>& Value)
{
    FPostHogFeatureFlags Out; Out.Status = EPostHogResult::Ok;
    if (Value && Value->IsNull()) { return Out; }
    if (!Value || Value->Type != EJson::Object) { Out.Status = EPostHogResult::InvalidResponse; return Out; }
    for (const auto& Pair : Value->AsObject()->Values)
    {
        auto Record = Flag(Pair.Value);
        if (Record.Status != EPostHogResult::Ok || !Record.bFound)
        { Out.Status = EPostHogResult::InvalidResponse; Out.Flags.Empty(); return Out; }
        Out.Flags.Add(FString(Pair.Key), MoveTemp(Record));
    }
    Out.bAvailable = true;
    return Out;
}
FPostHogFeatureFlagsUpdate PostHogNative::Update(const TSharedPtr<FJsonValue>& Value)
{
    FPostHogFeatureFlagsUpdate Out;
    Out.Snapshot.Status = EPostHogResult::InvalidResponse;
    if (!Value || Value->Type != EJson::Object) { return Out; }
    const auto Object = Value->AsObject();
    const TSharedPtr<FJsonObject>* Context = nullptr;
    const auto Error = Object->TryGetField(TEXT("error"));
    if (!Object->TryGetObjectField(TEXT("context"), Context) || !Error ||
        !Object->TryGetBoolField(TEXT("errors_loading"), Out.bErrorsLoading) ||
        !Object->TryGetBoolField(TEXT("partial"), Out.bPartial) ||
        !Object->TryGetBoolField(TEXT("invalid_payloads"), Out.bInvalidPayloads)) { return Out; }
    Out.Context = FPostHogProperties::FromNative(Context->ToSharedRef(), PHU_MAX_OUTPUT);
    if (!Out.Context.IsValid() || !FPostHogValue::TryParse(PostHogJson::EncodeValue(Error), Out.Error, PHU_MAX_OUTPUT)) { return Out; }
    Out.Snapshot = Flags(Object->TryGetField(TEXT("flags")));
    return Out;
}
