#include "PostHogSubsystem.h"
#include "PostHogAnalyticsProvider.h"
#include "PostHogJson.h"
#include "PostHogTasks.h"
#include "PostHogReplayRecorder.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/EngineVersion.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/StrongObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogSubsystem, Log, All);

/** Plain-data lifetime handoff; no UObject references enter workers. */
struct FPostHogInitialization
{
    FCriticalSection Mutex;
    bool bCancelled = false;
#if WITH_DEV_AUTOMATION_TESTS
    bool bWaitingForPredecessor = false;
#endif
    FPostHogClient::FPtr Client;
    TSharedFuture<bool> Ready;
    TSharedFuture<FPostHogDrainResult> Closing;
    void Cancel()
    {
        FPostHogClient::FPtr Current;
        { FScopeLock Lock(&Mutex); bCancelled = true; Current = Client; }
        if (Current)
        {
            const auto Future = Current->ShutdownAsync();
            FScopeLock Lock(&Mutex); Closing = Future;
        }
    }
    FPostHogDrainResult Join()
    {
        Ready.Wait();
        Cancel();
        TSharedFuture<FPostHogDrainResult> Future;
        { FScopeLock Lock(&Mutex); Future = Closing; }
        if (Future.IsValid()) { return Future.Get(); }
        // Initialization was cancelled before it owned a native client.
        FPostHogDrainResult Empty; Empty.Status = EPostHogResult::Ok; Empty.bJoined = true; return Empty;
    }
    bool Released()
    {
        FScopeLock Lock(&Mutex);
        return bCancelled && Ready.IsValid() && Ready.IsReady() &&
            (!Client || (Closing.IsValid() && Closing.IsReady() && Closing.Get().bJoined));
    }
};
struct FPostHogLifecycleSignals
{
    FCriticalSection Mutex;
    TArray<bool> Pending;
    void Push(bool bForeground)
    {
        FScopeLock Lock(&Mutex);
        if (!Pending.IsEmpty() && Pending.Last() == bForeground) { return; }
        if (Pending.Num() < 32) { Pending.Add(bForeground); } else { Pending.Last() = bForeground; }
    }
    TArray<bool> Take()
    { FScopeLock Lock(&Mutex); TArray<bool> Out = MoveTemp(Pending); Pending.Empty(); return Out; }
};
namespace
{
// Engine ownership only: do not reuse a cancelled namespace until native close has joined.
TMap<FString, TSharedPtr<FPostHogInitialization, ESPMode::ThreadSafe>> Owners;
void RetireClosedOwners()
{
    check(IsInGameThread());
    for (auto It = Owners.CreateIterator(); It; ++It) { if (It.Value()->Released()) { It.RemoveCurrent(); } }
}
FPostHogProperties SampleContext()
{
    FPostHogProperties Context;
    FString OS, OSVersion; FPlatformMisc::GetOSVersions(OS, OSVersion);
    Context.SetString(TEXT("$os"), OS); Context.SetString(TEXT("$os_version"), FPlatformMisc::GetOSVersion());
    Context.SetString(TEXT("$engine_version"), FEngineVersion::Current().ToString());
    Context.SetString(TEXT("$app_name"), FApp::GetProjectName());
    Context.SetString(TEXT("$locale"), FPlatformMisc::GetDefaultLocale());
    Context.SetString(TEXT("$device_type"), TEXT("Desktop"));
    FString Version;
    if (GConfig && GConfig->GetString(TEXT("/Script/EngineSettings.GeneralProjectSettings"), TEXT("ProjectVersion"), Version, GGameIni) && !Version.IsEmpty())
    { Context.SetString(TEXT("$app_version"), Version); }
    return Context;
}
}
void UPostHogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    const auto* Settings = GetDefault<UPostHogSettings>();
    if (Settings->bAutoInitialize && !Settings->Options.ProjectToken.IsEmpty()) { Setup(Settings->Options); }
}
bool UPostHogSubsystem::Setup(const FPostHogOptions& Options)
{ return SetupWithConfig(Options.ToNative(), Options.bEnableInEditor, Options.bEnableOnDedicatedServer); }
bool UPostHogSubsystem::SetupWithConfig(FPostHogConfig Config, bool bEnableInEditor, bool bEnableOnDedicatedServer)
{
    if (!IsInGameThread() || (State != EPostHogSubsystemState::Idle && State != EPostHogSubsystemState::Failed)) { return false; }
    auto* Instance = GetGameInstance(); const auto* WorldContext = Instance ? Instance->GetWorldContext() : nullptr;
    if (!WorldContext || (WorldContext->WorldType != EWorldType::Game && WorldContext->WorldType != EWorldType::PIE) ||
        IsRunningCommandlet() || (GIsEditor && !bEnableInEditor) || (Instance->IsDedicatedServerInstance() && !bEnableOnDedicatedServer)) { return false; }
    const FString Scope = WorldContext->WorldType == EWorldType::PIE ? FString::Printf(TEXT("editor/pie-%d"), WorldContext->PIEInstance) :
        (GIsEditor ? FString(TEXT("editor/standalone")) : FString(TEXT("runtime")));
    Config.ClientName = Scope + TEXT("/") + Config.ClientName;
    if (Config.StorageRoot.IsEmpty()) { Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogStateful"); }
    auto Context = SampleContext().ToNative(); PostHogJson::Merge(*Context, *Config.Context.ToNative());
    Config.Context = FPostHogProperties::FromNative(Context); if (!Config.Context.IsValid()) { return false; }
    FString ScopeKey;
    if (FPostHogClient::CoordinationKey(Config, ScopeKey) != EPostHogResult::Ok) { return false; }
    const FString Key = FPaths::ConvertRelativePathToFull(Config.StorageRoot) / ScopeKey;
    RetireClosedOwners();
    auto Previous = Owners.FindRef(Key);
    if (Previous) { FScopeLock Lock(&Previous->Mutex); if (!Previous->bCancelled) { return false; } }
    ReplaySettings = Config.Replay;
    bCaptureApplicationLifecycleEvents = Config.bCaptureApplicationLifecycleEvents;
    State = EPostHogSubsystemState::Initializing;
    StartObserving(); EnsureTicker();
    auto Request = MakeShared<FPostHogInitialization, ESPMode::ThreadSafe>(); Initialization = Request; Owners.Add(Key, Request);
    Request->Ready = PostHogTasks::Run([Request, Previous, Config = MoveTemp(Config)]() mutable
    {
        if (Previous)
        {
#if WITH_DEV_AUTOMATION_TESTS
            { FScopeLock Lock(&Request->Mutex); Request->bWaitingForPredecessor = true; }
#endif
            const bool bJoined = Previous->Join().bJoined;
#if WITH_DEV_AUTOMATION_TESTS
            { FScopeLock Lock(&Request->Mutex); Request->bWaitingForPredecessor = false; }
#endif
            if (!bJoined) { return false; }
        }
        { FScopeLock Lock(&Request->Mutex); if (Request->bCancelled) { return false; } }
        auto Created = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
        bool bCancelled;
        { FScopeLock Lock(&Request->Mutex); Request->Client = Created; bCancelled = Request->bCancelled; }
        if (bCancelled) { Request->Cancel(); return false; }
        return Created.IsValid();
    });
    return true;
}
void UPostHogSubsystem::RetireInitializationOwners()
{
    check(IsInGameThread());
    // ShutdownAll has joined the workers: no initializer can publish another client.
    for (const auto& Owner : Owners) { Owner.Value->Cancel(); }
    for (const auto& Owner : Owners)
    {
        if (!Owner.Value->Released())
        { UE_LOG(LogPostHogSubsystem, Fatal, TEXT("PostHog cannot retire an initialization owner before joined shutdown.")); }
    }
    // Destroy registry-held clients and pooled-event futures before Unreal services shut down.
    RetireClosedOwners();
}
#if WITH_DEV_AUTOMATION_TESTS
TWeakPtr<FPostHogInitialization, ESPMode::ThreadSafe> UPostHogSubsystem::GetInitializationForTests() const
{ check(IsInGameThread()); return Initialization; }
void UPostHogSubsystem::RetireClosedInitializationOwnersForTests() { RetireClosedOwners(); }
bool UPostHogSubsystem::IsWaitingForPredecessorForTests() const
{
    if (!IsInGameThread() || !Initialization) { return false; }
    FScopeLock Lock(&Initialization->Mutex); return Initialization->bWaitingForPredecessor;
}
#endif
bool UPostHogSubsystem::IsInitialized() const
{ return IsInGameThread() && State == EPostHogSubsystemState::Ready && Client && Client->IsInitialized(); }
bool UPostHogSubsystem::IsDisabled() const { auto Current = GetClient(); return !Current || Current->IsDisabled(); }
FPostHogClient::FPtr UPostHogSubsystem::GetClient() const { return IsInitialized() ? Client : nullptr; }
EPostHogResult UPostHogSubsystem::Capture(const FString& Event, const FPostHogProperties& Properties)
{ auto Current = GetClient(); return Current ? Current->Capture(Event, Properties) : EPostHogResult::Closed; }
EPostHogResult UPostHogSubsystem::Identify(const FString& Id, const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{ auto Current = GetClient(); return Current ? Current->Identify(Id, Set, SetOnce) : EPostHogResult::Closed; }
EPostHogResult UPostHogSubsystem::Reset() { auto Current = GetClient(); return Current ? Current->Reset() : EPostHogResult::Closed; }
EPostHogResult UPostHogSubsystem::OptIn() { auto Current = GetClient(); return Current ? Current->OptIn() : EPostHogResult::Closed; }
EPostHogResult UPostHogSubsystem::OptOut() { auto Current = GetClient(); return Current ? Current->OptOut() : EPostHogResult::Closed; }
FPostHogFeatureFlagResult UPostHogSubsystem::GetFeatureFlag(const FString& Key, EPostHogFlagReporting Reporting)
{ auto Current = GetClient(); return Current ? Current->GetFeatureFlagResult(Key, Reporting) : FPostHogFeatureFlagResult(); }
FPostHogFeatureFlags UPostHogSubsystem::GetFeatureFlags() const
{ auto Current = GetClient(); return Current ? Current->GetFeatureFlags() : FPostHogFeatureFlags(); }
EPostHogResult UPostHogSubsystem::ReloadFeatureFlags()
{ auto Current = GetClient(); return Current ? Current->ReloadFeatureFlags() : EPostHogResult::Closed; }
TSharedPtr<IAnalyticsProvider> UPostHogSubsystem::GetAnalyticsProvider()
{
    auto Current = GetClient(); if (!Current) { return nullptr; }
    if (!AnalyticsProvider) { AnalyticsProvider = MakeShareable(new FPostHogAnalyticsProvider(Current)); }
    return AnalyticsProvider;
}
bool UPostHogSubsystem::BindAnalyticsProvider()
{ return GetAnalyticsProvider() && FPostHogAnalyticsProvider::Bind(AnalyticsProvider); }
void UPostHogSubsystem::UnbindAnalyticsProvider()
{ if (IsInGameThread() && AnalyticsProvider) { AnalyticsProvider->Unbind(); } }
void UPostHogSubsystem::Shutdown()
{
    if (!IsInGameThread() || State == EPostHogSubsystemState::ShuttingDown || State == EPostHogSubsystemState::Deinitialized) { return; }
    UnbindAnalyticsProvider(); AnalyticsProvider.Reset();
    StopObserving();
    if (Initialization)
    {
        auto Request = Initialization; Request->Cancel(); Initialization.Reset(); Client.Reset();
        auto Promise = MakeShared<TPromise<FPostHogDrainResult>, ESPMode::ThreadSafe>(); Closing = Promise->GetFuture().Share();
        if (!PostHogTasks::Launch([Request, Promise] { Promise->SetValue(Request->Join()); }))
        { FPostHogDrainResult Error; Error.Status = EPostHogResult::TaskUnavailable; Promise->SetValue(Error); }
    }
    else if (Client) { Closing = Client->ShutdownAsync(); Client.Reset(); }
    else { State = EPostHogSubsystemState::Idle; return; }
    Recorder.Reset();
    State = EPostHogSubsystemState::ShuttingDown; EnsureTicker();
}
void UPostHogSubsystem::EnsureTicker()
{
    if (!TickerHandle.IsValid())
    { TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float Delta) { return Tick(Delta); })); }
}
bool UPostHogSubsystem::Tick(float)
{
    if (!IsInGameThread()) { return true; }
    // Lifecycle hooks/delegates can deinitialize the owner and collect garbage inline.
    TStrongObjectPtr<UPostHogSubsystem> KeepAlive(this);
    if (Signals)
    {
        for (bool bNext : Signals->Take())
        {
            if (bForeground == bNext) { continue; }
            bForeground = bNext;
            if (auto Current = GetClient())
            {
                if (Recorder) { Recorder->SetAvailable(bForeground); }
                // Open after the SDK enters foreground; close the foreground event before backgrounding.
                if (bForeground) { Current->SetAppInBackground(false); }
                if (GetClient() != Current) { break; }
                if (bCaptureApplicationLifecycleEvents)
                {
                    FPostHogProperties Properties;
                    if (bForeground) { Properties.SetBoolean(TEXT("from_background"), true); }
                    Current->Capture(bForeground ? TEXT("Application Opened") : TEXT("Application Backgrounded"), Properties);
                    if (GetClient() != Current) { break; }
                }
                if (!bForeground) { Current->SetAppInBackground(true); }
            }
        }
    }
    if (State == EPostHogSubsystemState::Initializing && Initialization && Initialization->Ready.IsReady())
    {
        const bool bSuccess = Initialization->Ready.Get();
        { FScopeLock Lock(&Initialization->Mutex); Client = Initialization->Client; }
        State = bSuccess && Client ? EPostHogSubsystemState::Ready : EPostHogSubsystemState::Failed;
        if (State == EPostHogSubsystemState::Ready)
        {
            auto Current = Client;
            Current->SetAppInBackground(!bForeground);
            if (GetClient() == Current)
            {
                Recorder = FPostHogReplayRecorder::Attach(GetGameInstance(), Current, ReplaySettings);
                if (Recorder) { Recorder->SetAvailable(bForeground); }
                if (bCaptureApplicationLifecycleEvents && bForeground)
                {
                    FPostHogProperties Properties; Properties.SetBoolean(TEXT("from_background"), false);
                    Current->Capture(TEXT("Application Opened"), Properties);
                }
                if (GetClient() == Current) { OnSetupCompleted.Broadcast(true); }
            }
        }
        else { Initialization->Cancel(); Initialization.Reset(); StopObserving(); OnSetupCompleted.Broadcast(false); }
    }
    if (State == EPostHogSubsystemState::Ready && Client)
    {
        auto Current = Client; FPostHogFeatureFlagsUpdate Update;
        const auto Status = Current->IsInitialized() ? Current->PollFeatureFlags(Update) : EPostHogResult::Closed;
        if (Status != EPostHogResult::Inactive)
        {
            // These are SDK snapshots, not targeting-generation validation in the adapter.
            OnFeatureFlagsChanged.Broadcast(Update);
            if (Status == EPostHogResult::Closed && Client == Current && State == EPostHogSubsystemState::Ready)
            { if (Recorder) { Recorder->Stop(); Recorder.Reset(); } State = EPostHogSubsystemState::Failed; StopObserving(); }
        }
    }
    if (State == EPostHogSubsystemState::ShuttingDown && Closing.IsValid() && Closing.IsReady())
    {
        const auto Result = Closing.Get(); Closing = {};
        State = Result.bJoined ? EPostHogSubsystemState::Idle : EPostHogSubsystemState::Failed;
        OnShutdownCompleted.Broadcast(Result);
    }
    if (State == EPostHogSubsystemState::Idle || State == EPostHogSubsystemState::Failed || State == EPostHogSubsystemState::Deinitialized)
    { TickerHandle.Reset(); return false; }
    return true;
}
void UPostHogSubsystem::StartObserving()
{
    Signals = MakeShared<FPostHogLifecycleSignals, ESPMode::ThreadSafe>(); auto Inbox = Signals;
    BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda([Inbox] { Inbox->Push(false); });
    ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddLambda([Inbox] { Inbox->Push(true); });
    DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddLambda([Inbox] { Inbox->Push(false); });
    ReactivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddLambda([Inbox] { Inbox->Push(true); });
    if (FSlateApplication::IsInitialized())
    {
        bForeground = FSlateApplication::Get().IsActive();
        SlateHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddLambda([Inbox](bool bActive) { Inbox->Push(bActive); });
    }
    else { bForeground = true; }
    ExitHandle = FCoreDelegates::OnEnginePreExit.AddUObject(this, &UPostHogSubsystem::HandleEngineExit);
}
void UPostHogSubsystem::StopObserving()
{
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
    FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(ReactivateHandle);
    if (SlateHandle.IsValid() && FSlateApplication::IsInitialized()) { FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(SlateHandle); }
    FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
    BackgroundHandle.Reset(); ForegroundHandle.Reset(); DeactivateHandle.Reset(); ReactivateHandle.Reset(); SlateHandle.Reset(); ExitHandle.Reset(); Signals.Reset();
}
EPostHogResult UPostHogSubsystem::StartSessionReplay()
{ auto Current = GetClient(); return Current ? Current->StartSessionReplay() : EPostHogResult::Closed; }
EPostHogResult UPostHogSubsystem::StopSessionReplay()
{ auto Current = GetClient(); return Current ? Current->StopSessionReplay() : EPostHogResult::Closed; }
bool UPostHogSubsystem::IsSessionReplayActive() const
{ auto Current = GetClient(); return Current && Current->IsSessionReplayActive(); }
EPostHogResult UPostHogSubsystem::BeginReplayFrame(int32 Width, int32 Height, int32 Stride, UPostHogReplayFrame*& Frame)
{
    Frame = nullptr; auto Current = GetClient(); if (!Current) { return EPostHogResult::Closed; }
    TUniquePtr<FPostHogReplayFrame> Lease; const auto Result = Current->BeginReplayFrame(Width, Height, Stride, Lease);
    if (Result == EPostHogResult::Ok) { Frame = NewObject<UPostHogReplayFrame>(this); Frame->Lease = MoveTemp(Lease); }
    return Result;
}
void UPostHogSubsystem::Stop()
{
    if (State == EPostHogSubsystemState::Deinitialized) { return; }
    Shutdown(); StopObserving(); FTSTicker::RemoveTicker(TickerHandle); TickerHandle.Reset(); State = EPostHogSubsystemState::Deinitialized;
}
void UPostHogSubsystem::HandleEngineExit() { Stop(); }
void UPostHogSubsystem::Deinitialize() { Stop(); Super::Deinitialize(); }
