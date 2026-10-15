#include "PostHogFlagCache.h"
#include "PostHogJson.h"

namespace
{
bool ParseRecords(const FJsonObject& Records, TMap<FString, FPostHogFeatureFlagResult>& Out)
{
    for (const auto& Pair : Records.Values)
    {
        if (Pair.Value->Type != EJson::Object) { return false; }
        const auto Record = Pair.Value->AsObject();
        const auto Value = Record->TryGetField(TEXT("value"));
        if (!Value || (Value->Type != EJson::Boolean && Value->Type != EJson::String)) { return false; }
        FPostHogFeatureFlagResult Flag;
        Flag.bFound = true;
        if (!FPostHogValue::TryParse(PostHogJson::EncodeValue(Value), Flag.Value)) { return false; }
        if (const auto Payload = Record->TryGetField(TEXT("payload")))
        {
            // Native export already decoded payloads. A string must stay a string.
            Flag.bHasPayload = FPostHogValue::TryParse(PostHogJson::EncodeValue(Payload), Flag.Payload);
        }
        auto Metadata = MakeShared<FJsonObject>();
        Metadata->Values = Record->Values;
        Metadata->RemoveField(TEXT("value")); Metadata->RemoveField(TEXT("payload"));
        Flag.Metadata = FPostHogProperties::FromNative(Metadata);
        if (!Flag.Metadata.IsValid()) { return false; }
        Out.Add(FString(Pair.Key), MoveTemp(Flag));
    }
    return true;
}
}

void FPostHogFlagCache::Clear()
{
    Flags.Empty(); Reported.Empty(); SessionRecording.Reset(); bLoaded = false; ++Revision;
}

EPostHogFlagReloadStatus FPostHogFlagCache::Apply(const TSharedRef<FJsonObject>& Response)
{
    bool bPartial = false, bQuota = false;
    const TSharedPtr<FJsonObject>* Records = nullptr;
    if (!Response->TryGetBoolField(TEXT("errors_while_computing"), bPartial) ||
        !Response->TryGetBoolField(TEXT("quota_limited"), bQuota)) { return EPostHogFlagReloadStatus::Failed; }
    if (bQuota) { return EPostHogFlagReloadStatus::QuotaLimited; }
    if (!Response->TryGetObjectField(TEXT("flags"), Records)) { return EPostHogFlagReloadStatus::Failed; }
    TMap<FString, FPostHogFeatureFlagResult> Incoming;
    if (!ParseRecords(**Records, Incoming)) { return EPostHogFlagReloadStatus::Failed; }
    for (auto& Pair : Incoming)
    {
        auto Metadata = Pair.Value.Metadata.ToNative();
        for (const FString& Key : {FString(TEXT("request_id")), FString(TEXT("evaluated_at"))})
        {
            if (auto Value = Response->TryGetField(Key)) { Metadata->SetField(Key, Value); }
        }
        Metadata->SetBoolField(TEXT("errors_while_computing"), bPartial);
        Pair.Value.Metadata = FPostHogProperties::FromNative(Metadata);
    }
    auto Previous = Flags;
    if (!bPartial) { Flags.Empty(); }
    for (auto& Pair : Incoming) { Flags.Add(Pair.Key, MoveTemp(Pair.Value)); }
    // Partial responses cannot grow the cache beyond the JSON admission bound.
    if (!Serialize().IsValid()) { Flags = MoveTemp(Previous); return EPostHogFlagReloadStatus::Failed; }
    // Only an object enables replay; false, null, or a missing field disables it.
    const TSharedPtr<FJsonObject>* Recording = nullptr;
    if (Response->TryGetObjectField(TEXT("session_recording"), Recording)) { RestoreSessionRecording(*Recording); }
    else { SessionRecording.Reset(); }
    bLoaded = true; Reported.Empty(); ++Revision;
    return bPartial ? EPostHogFlagReloadStatus::Partial : EPostHogFlagReloadStatus::Success;
}

void FPostHogFlagCache::RestoreSessionRecording(const TSharedPtr<FJsonObject>& Saved)
{
    SessionRecording.Reset();
    auto Config = FPostHogProperties::FromNative(Saved.ToSharedRef());
    if (Config.IsValid()) { SessionRecording = MoveTemp(Config); }
}

FPostHogProperties FPostHogFlagCache::Serialize() const
{
    auto Records = MakeShared<FJsonObject>();
    for (const auto& Pair : Flags)
    {
        auto Record = Pair.Value.Metadata.ToNative();
        Record->SetField(TEXT("value"), Pair.Value.Value.ToNative());
        if (Pair.Value.bHasPayload) { Record->SetField(TEXT("payload"), Pair.Value.Payload.ToNative()); }
        Records->SetObjectField(Pair.Key, Record);
    }
    return FPostHogProperties::FromNative(Records);
}

bool FPostHogFlagCache::Restore(const FPostHogProperties& Saved)
{
    TMap<FString, FPostHogFeatureFlagResult> Incoming;
    if (!Saved.IsValid() || !ParseRecords(*Saved.ToNative(), Incoming)) { return false; }
    Flags = MoveTemp(Incoming); bLoaded = true; Reported.Empty(); ++Revision;
    return true;
}

FPostHogProperties FPostHogFlagCache::Values() const
{
    auto Result = MakeShared<FJsonObject>();
    for (const auto& Pair : Flags) { Result->SetField(Pair.Key, Pair.Value.Value.ToNative()); }
    return FPostHogProperties::FromNative(Result);
}

FPostHogProperties FPostHogFlagCache::Exposure(const FString& Key, const FPostHogFeatureFlagResult& Flag)
{
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("$feature_flag"), Key);
    Properties->SetField(TEXT("$feature_flag_response"), Flag.Value.ToNative());
    const auto Metadata = Flag.Metadata.ToNative();
    const TPair<const TCHAR*, const TCHAR*> Fields[] = {
        {TEXT("id"), TEXT("$feature_flag_id")}, {TEXT("version"), TEXT("$feature_flag_version")},
        {TEXT("reason"), TEXT("$feature_flag_reason")}, {TEXT("request_id"), TEXT("$feature_flag_request_id")},
        {TEXT("evaluated_at"), TEXT("$feature_flag_evaluated_at")},
        {TEXT("has_experiment"), TEXT("$feature_flag_has_experiment")}, {TEXT("locally_evaluated"), TEXT("locally_evaluated")}
    };
    for (const auto& Field : Fields)
    {
        if (auto Value = Metadata->TryGetField(Field.Key); Value && !Value->IsNull()) { Properties->SetField(Field.Value, Value); }
    }
    bool bPartial = false;
    if (Metadata->TryGetBoolField(TEXT("errors_while_computing"), bPartial) && bPartial)
    { Properties->SetStringField(TEXT("$feature_flag_error"), TEXT("errors_while_computing_flags")); }
    return FPostHogProperties::FromNative(Properties);
}

bool FPostHogFlagCache::IsMinimal(const FPostHogFeatureFlagResult& Flag)
{
    const auto Metadata = Flag.Metadata.ToNative();
    bool bGate = false, bExperiment = true;
    return Metadata->TryGetBoolField(TEXT("minimal_flag_called_events"), bGate) && bGate &&
        Metadata->TryGetBoolField(TEXT("has_experiment"), bExperiment) && !bExperiment;
}

void FPostHogFlagCache::Minimize(FPostHogProperties& Properties)
{
    static const TSet<FString> Allowed = {
        TEXT("$feature_flag"), TEXT("$feature_flag_response"), TEXT("$feature_flag_has_experiment"),
        TEXT("$feature_flag_id"), TEXT("$feature_flag_version"), TEXT("$feature_flag_reason"),
        TEXT("$feature_flag_request_id"), TEXT("$feature_flag_evaluated_at"), TEXT("$feature_flag_error"),
        TEXT("locally_evaluated"), TEXT("$groups"), TEXT("$process_person_profile"), TEXT("$session_id"),
        TEXT("$lib"), TEXT("$lib_version"), TEXT("$os"), TEXT("$os_version"), TEXT("$device_type"),
        TEXT("$engine_version"), TEXT("$geoip_disable"), TEXT("$referring_domain"),
        TEXT("utm_source"), TEXT("utm_medium"), TEXT("utm_campaign"), TEXT("utm_content"), TEXT("utm_term"),
        TEXT("utm_id"), TEXT("utm_source_platform"), TEXT("utm_creative_format"), TEXT("utm_marketing_tactic"),
        TEXT("gad_source"), TEXT("gclid"), TEXT("dclid"), TEXT("gbraid"), TEXT("wbraid"), TEXT("fbclid"),
        TEXT("msclkid"), TEXT("twclid"), TEXT("ttclid"), TEXT("li_fat_id"), TEXT("mc_cid"), TEXT("igshid"), TEXT("irclid")
    };
    auto Object = Properties.ToNative();
    for (auto It = Object->Values.CreateIterator(); It; ++It) { if (!Allowed.Contains(FString(It.Key()))) { It.RemoveCurrent(); } }
    Properties = FPostHogProperties::FromNative(Object);
}
