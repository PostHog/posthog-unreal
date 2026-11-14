#include "PostHogAnalyticsProvider.h"
#include "PostHogSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/ScopeLock.h"
#include "Misc/ScopeExit.h"

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
    return ConvertAttributes(GetDefaultEventAttributesSafe());
}

bool FPostHogAnalyticsProvider::StartSession(const TArray<FAnalyticsEventAttribute>& Attributes)
{
    auto Current = Client; auto State = Annotations;
    const auto Properties = ConvertAttributes(Attributes), Defaults = GetDefaults();
    if (!Current->IsInitialized() || Current->IsOptedOut()) { return false; }
    bool bEnding;
    { FScopeLock Lock(&State->Mutex); bEnding = State->bEnding; }
    // A callback may start a new native session while its previous end annotation is in flight.
    if (bEnding && Current->EndSession() != EPostHogResult::Ok) { return false; }
    if (Current->StartSession() != EPostHogResult::Ok) { return false; }
    const FString Session = Current->GetSessionId(); if (Session.IsEmpty()) { return false; }
    {
        FScopeLock Lock(&State->Mutex);
        if (State->Started == Session) { return !State->bStarting; }
        State->Started = Session; State->bStarting = true;
    }
    const auto Status = Current->CaptureWithFallback(TEXT("SessionStart"), Properties, Defaults);
    const bool bAccepted = Status == EPostHogResult::Accepted && Current->IsInitialized() && Current->PeekSessionId() == Session;
    {
        FScopeLock Lock(&State->Mutex);
        if (State->Started == Session)
        { State->bStarting = false; if (!bAccepted) { State->Started.Empty(); } }
    }
    return bAccepted;
}
void FPostHogAnalyticsProvider::EndSession()
{
    auto Current = Client; auto State = Annotations; const auto Defaults = GetDefaults();
    {
        FScopeLock Lock(&State->Mutex); if (State->bEnding) { return; }
        State->bEnding = true; State->Started.Empty(); State->bStarting = false;
    }
    ON_SCOPE_EXIT { FScopeLock Lock(&State->Mutex); State->bEnding = false; };
    // Annotate the observed session even if SDK maintenance expires it before capture.
    const FString EndingId = Current->PeekSessionId();
    const FString CurrentId = EndingId.IsEmpty() ? FString() : Current->GetSessionId();
    if (!EndingId.IsEmpty())
    {
        FPostHogProperties Properties; Properties.SetString(TEXT("$session_id"), EndingId);
        Current->CaptureWithFallback(TEXT("SessionEnd"), Properties, Defaults);
    }
    if (Current->IsInitialized() && Current->PeekSessionId() == CurrentId) { Current->EndSession(); }
    if (Current->IsInitialized()) { Current->FlushAsync(); }
}
FString FPostHogAnalyticsProvider::GetSessionID() const { return Client->GetSessionId(); }
bool FPostHogAnalyticsProvider::SetSessionID(const FString&) { return false; }
void FPostHogAnalyticsProvider::FlushEvents() { auto Current = Client; Current->FlushAsync(); }
void FPostHogAnalyticsProvider::SetUserID(const FString& InUserID) { auto Current = Client; Current->Identify(InUserID); }
FString FPostHogAnalyticsProvider::GetUserID() const { return Client->IsInitialized() ? Client->GetDistinctId() : FString(); }

void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes)
{
    auto Current = Client; const auto Defaults = GetDefaults();
    Current->CaptureWithFallback(EventName, ConvertAttributes(Attributes), Defaults);
}
void FPostHogAnalyticsProvider::RecordEvent(const FString& EventName, const TArray<FAnalyticsEventAttribute>& Attributes, EAnalyticsRecordEventMode Mode)
{
    auto Current = Client; const auto Defaults = GetDefaults();
    Current->CaptureWithFallback(EventName, ConvertAttributes(Attributes), Defaults);
    if (Mode == EAnalyticsRecordEventMode::Immediate && Current->IsInitialized()) { Current->FlushAsync(); }
}

void FPostHogAnalyticsProvider::SetDefaultEventAttributes(TArray<FAnalyticsEventAttribute>&& Attributes)
{
    FScopeLock Lock(&DefaultsMutex);
    DefaultAttributes = MoveTemp(Attributes);
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
