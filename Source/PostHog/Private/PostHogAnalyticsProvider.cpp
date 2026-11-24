#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogAnalytics, Log, All);

namespace
{
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
        else
        {
            Value = FPostHogValue::String(Attribute.GetValue());
        }
        if (!Properties.Set(Attribute.GetName(), Value))
        {
            UE_LOG(LogPostHogAnalytics, Warning, TEXT("Ignoring an Analytics attribute that exceeds property bounds."));
        }
    }
    return Properties;
}

FPostHogProperties FPostHogAnalyticsProvider::GetDefaults() const
{
    FScopeLock Lock(&DefaultsMutex);
    return CachedDefaults;
}

bool FPostHogAnalyticsProvider::StartSession(const TArray<FAnalyticsEventAttribute>& Attributes)
{
    auto Current = Client;
    if (!Current->IsInitialized() || Current->IsOptedOut()) { return false; }
    Current->StartSession();
    const FString Session = Current->GetSessionId();
    if (Session.IsEmpty()) { return false; }
    {
        FScopeLock Lock(&SessionMutex);
        if (StartedSessionId == Session) { return true; }
        StartedSessionId = Session;
    }
    const auto Result = Current->CaptureWithFallback(TEXT("SessionStart"), ConvertAttributes(Attributes), GetDefaults());
    const bool bAccepted = Result == EPostHogCaptureResult::Accepted && Current->IsInitialized() && Current->PeekSessionId() == Session;
    if (!bAccepted)
    {
        FScopeLock Lock(&SessionMutex);
        if (StartedSessionId == Session) { StartedSessionId.Empty(); }
    }
    return bAccepted;
}

void FPostHogAnalyticsProvider::EndSession()
{
    auto Current = Client;
    {
        FScopeLock Lock(&SessionMutex);
        StartedSessionId.Empty();
    }
    const FString EndingId = Current->PeekSessionId();
    const FString CurrentId = EndingId.IsEmpty() ? FString() : Current->GetSessionId();
    if (!EndingId.IsEmpty())
    {
        FPostHogProperties Properties;
        Properties.SetString(TEXT("$session_id"), EndingId);
        Current->CaptureWithFallback(TEXT("SessionEnd"), Properties, GetDefaults());
    }
    if (Current->IsInitialized() && Current->PeekSessionId() == CurrentId) { Current->EndSession(); }
    if (Current->IsInitialized()) { Current->FlushAsync(); }
}

FString FPostHogAnalyticsProvider::GetSessionID() const { return Client->GetSessionId(); }
bool FPostHogAnalyticsProvider::SetSessionID(const FString&) { return false; }
void FPostHogAnalyticsProvider::FlushEvents() { Client->FlushAsync(); }
void FPostHogAnalyticsProvider::SetUserID(const FString& InUserID) { Client->Identify(InUserID); }
FString FPostHogAnalyticsProvider::GetUserID() const { return Client->IsInitialized() ? Client->GetDistinctId() : FString(); }

void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes)
{
    Client->CaptureWithFallback(EventName, ConvertAttributes(Attributes), GetDefaults());
}

void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes, EAnalyticsRecordEventMode Mode)
{
    Client->CaptureWithFallback(EventName, ConvertAttributes(Attributes), GetDefaults());
    if (Mode == EAnalyticsRecordEventMode::Immediate && Client->IsInitialized()) { Client->FlushAsync(); }
}

void FPostHogAnalyticsProvider::SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes)
{
    FPostHogProperties Converted = ConvertAttributes(Attributes);
    FScopeLock Lock(&DefaultsMutex);
    DefaultAttributes = MoveTemp(Attributes);
    CachedDefaults = MoveTemp(Converted);
}

TArray<FAnalyticsEventAttribute> FPostHogAnalyticsProvider::GetDefaultEventAttributesSafe() const
{
    FScopeLock Lock(&DefaultsMutex);
    return DefaultAttributes;
}

int32 FPostHogAnalyticsProvider::GetDefaultEventAttributeCount() const
{
    FScopeLock Lock(&DefaultsMutex);
    return DefaultAttributes.Num();
}

FAnalyticsEventAttribute FPostHogAnalyticsProvider::GetDefaultEventAttribute(int AttributeIndex) const
{
    FScopeLock Lock(&DefaultsMutex);
    return DefaultAttributes.IsValidIndex(AttributeIndex) ? DefaultAttributes[AttributeIndex] : FAnalyticsEventAttribute();
}
