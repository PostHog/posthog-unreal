#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogAnalytics, Log, All);

namespace
{
// Game-thread-only, non-owning designation. Each subsystem owns its own adapter.
TWeakPtr<FPostHogAnalyticsProvider> BoundProvider;
}

TSharedPtr<IAnalyticsProvider> FPostHogAnalyticsProvider::GetForWorld(const UObject* WorldContextObject)
{
    if (!IsInGameThread() || !GEngine || !WorldContextObject) { return nullptr; }
    const auto* World = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull);
    const auto* Instance = World ? World->GetGameInstance() : nullptr;
    auto* Subsystem = Instance ? Instance->GetSubsystem<UPostHogSubsystem>() : nullptr;
    return Subsystem ? Subsystem->GetAnalyticsProvider() : nullptr;
}

TSharedPtr<IAnalyticsProvider> FPostHogAnalyticsProvider::GetBoundProvider()
{
    if (!IsInGameThread()) { return nullptr; }
    auto Provider = BoundProvider.Pin();
    return Provider && Provider->Client->IsInitialized() ? Provider : nullptr;
}

bool FPostHogAnalyticsProvider::Bind(const TSharedPtr<FPostHogAnalyticsProvider>& Provider)
{
    if (!IsInGameThread() || !Provider || !Provider->Client->IsInitialized()) { return false; }
    auto Existing = BoundProvider.Pin();
    if (Existing && Existing != Provider && Existing->Client->IsInitialized()) { return false; }
    BoundProvider = Provider;
    return true;
}

void FPostHogAnalyticsProvider::Unbind()
{
    if (IsInGameThread() && BoundProvider.Pin().Get() == this) { BoundProvider.Reset(); }
}

FPostHogProperties FPostHogAnalyticsProvider::ConvertAttributes(const TArray<FAnalyticsEventAttribute>& Attributes)
{
    FPostHogProperties Properties;
    for (const auto& Attribute : Attributes)
    {
        FPostHogValue Value;
        if (Attribute.IsJsonFragment())
        {
            if (!FPostHogValue::TryParse(Attribute.GetValue(), Value))
            {
                UE_LOG(LogPostHogAnalytics, Warning, TEXT("Ignoring an invalid Analytics JSON attribute."));
                continue;
            }
        }
        else { Value = FPostHogValue::String(Attribute.GetValue()); }
        if (!Properties.Set(Attribute.GetName(), Value))
        { UE_LOG(LogPostHogAnalytics, Warning, TEXT("Ignoring an Analytics attribute that exceeds property bounds.")); }
    }
    return Properties;
}

FPostHogProperties FPostHogAnalyticsProvider::GetDefaults() const
{
    FScopeLock Lock(&DefaultsMutex);
    return DefaultProperties;
}

bool FPostHogAnalyticsProvider::StartSession(const TArray<FAnalyticsEventAttribute>& Attributes)
{ return Client->StartAnalyticsSession(ConvertAttributes(Attributes), GetDefaults()); }
void FPostHogAnalyticsProvider::EndSession() { Client->EndAnalyticsSession(GetDefaults()); }
FString FPostHogAnalyticsProvider::GetSessionID() const { return Client->GetSessionId(); }
bool FPostHogAnalyticsProvider::SetSessionID(const FString& InSessionID) { return Client->SetSessionId(InSessionID); }
void FPostHogAnalyticsProvider::FlushEvents() { Client->FlushAsync(); }
void FPostHogAnalyticsProvider::SetUserID(const FString& InUserID) { Client->Identify(InUserID); }
FString FPostHogAnalyticsProvider::GetUserID() const { return Client->IsInitialized() ? Client->GetDistinctId() : FString(); }

void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes)
{
    const auto Defaults = GetDefaults();
    Client->CaptureWithDefaults(EventName, ConvertAttributes(Attributes), &Defaults);
}
void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes, EAnalyticsRecordEventMode Mode)
{
    RecordEvent(EventName, Attributes);
    if (Mode == EAnalyticsRecordEventMode::Immediate) { FlushEvents(); }
}

void FPostHogAnalyticsProvider::SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes)
{
    auto Properties = ConvertAttributes(Attributes);
    FScopeLock Lock(&DefaultsMutex);
    DefaultAttributes = MoveTemp(Attributes);
    DefaultProperties = MoveTemp(Properties);
}
TArray<FAnalyticsEventAttribute> FPostHogAnalyticsProvider::GetDefaultEventAttributesSafe() const
{ FScopeLock Lock(&DefaultsMutex); return DefaultAttributes; }
int32 FPostHogAnalyticsProvider::GetDefaultEventAttributeCount() const
{ FScopeLock Lock(&DefaultsMutex); return DefaultAttributes.Num(); }
FAnalyticsEventAttribute FPostHogAnalyticsProvider::GetDefaultEventAttribute(int AttributeIndex) const
{
    FScopeLock Lock(&DefaultsMutex);
    return DefaultAttributes.IsValidIndex(AttributeIndex) ? DefaultAttributes[AttributeIndex] : FAnalyticsEventAttribute();
}
