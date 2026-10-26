#include "PostHogSubsystem.h"
#include "PostHogJson.h"
#include "PostHogReplay.h"
#include "PostHogTasks.h"
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
#include "Misc/SecureHash.h"

/** No UObject references cross into initialization/close workers. */
struct FPostHogInitialization
{
    FCriticalSection Mutex;
    bool bCancelled = false;
    FPostHogClient::FPtr Client;
    TSharedFuture<bool> Ready;
    TSharedFuture<bool> Closing;

    void Cancel()
    {
        FScopeLock Lock(&Mutex); bCancelled = true;
        if (Client) { Closing = Client->ShutdownAsync(); Client.Reset(); }
    }
};

/** Platform callbacks only post plain data; UObject work is consumed by Tick. */
struct FPostHogLifecycleSignals
{
    FCriticalSection Mutex;
    TArray<bool> Pending;
    void Push(bool bForeground)
    {
        FScopeLock Lock(&Mutex);
        if (!Pending.IsEmpty() && Pending.Last() == bForeground) { return; }
        if (Pending.Num() < 32) { Pending.Add(bForeground); }
        else { Pending.Last() = bForeground; }
    }
    TArray<bool> Take()
    { FScopeLock Lock(&Mutex); TArray<bool> Result = MoveTemp(Pending); Pending.Empty(); return Result; }
};

namespace
{
// Game-thread-only weak leases serialize reuse of an initializing/closing namespace.
TMap<FString, TWeakPtr<FPostHogInitialization, ESPMode::ThreadSafe>> Owners;
FPostHogProperties SampleContext()
{
    FPostHogProperties Context;
    FString OS, OSVersion;
    FPlatformMisc::GetOSVersions(OS, OSVersion);
    Context.SetString(TEXT("$os"), OS);
    Context.SetString(TEXT("$os_version"), FPlatformMisc::GetOSVersion());
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
    auto* Instance = GetGameInstance();
    const auto* WorldContext = Instance ? Instance->GetWorldContext() : nullptr;
    if (!WorldContext || (WorldContext->WorldType != EWorldType::Game && WorldContext->WorldType != EWorldType::PIE) ||
        IsRunningCommandlet() || (GIsEditor && !bEnableInEditor) || (Instance->IsDedicatedServerInstance() && !bEnableOnDedicatedServer)) { return false; }
    if (Config.ProjectToken.TrimStartAndEnd().IsEmpty() || Config.Host.TrimStartAndEnd().IsEmpty() || Config.ClientName.TrimStartAndEnd().IsEmpty()) { return false; }
    const FString Scope = WorldContext->WorldType == EWorldType::PIE
        ? FString::Printf(TEXT("editor/pie-%d"), WorldContext->PIEInstance)
        : (GIsEditor ? FString(TEXT("editor/standalone")) : FString(TEXT("runtime")));
    Config.ClientName = Scope + TEXT("/") + Config.ClientName;
    if (Config.StorageRoot.IsEmpty()) { Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHog"); }
    auto Context = SampleContext().ToNative(); PostHogJson::Merge(*Context, *Config.Context.ToNative());
    Config.Context = FPostHogProperties::FromNative(Context);
    if (!Config.Context.IsValid()) { return false; }
    FString Host = Config.Host.TrimStartAndEnd();
    while (Host.EndsWith(TEXT("/"))) { Host.LeftChopInline(1); }
    const FString Key = FPaths::ConvertRelativePathToFull(Config.StorageRoot) / FMD5::HashAnsiString(*(Host + TEXT("\n") + Config.ProjectToken.TrimStartAndEnd() + TEXT("\n") + Config.ClientName));
    for (auto It = Owners.CreateIterator(); It; ++It) { if (!It.Value().IsValid()) { It.RemoveCurrent(); } }
    auto Previous = Owners.FindRef(Key).Pin();
    if (Previous) { FScopeLock Lock(&Previous->Mutex); if (!Previous->bCancelled) { return false; } }
    ReplaySettings = Config.Replay;
    State = EPostHogSubsystemState::Initializing;
    StartObserving(); EnsureTicker();
    auto Request = MakeShared<FPostHogInitialization, ESPMode::ThreadSafe>();
    Initialization = Request; Owners.Add(Key, Request);
    Request->Ready = PostHogTasks::Run([Request, Previous, Config = MoveTemp(Config)]() mutable
    {
        if (Previous)
        {
            Previous->Ready.Wait();
            TSharedFuture<bool> Closed;
            { FScopeLock Lock(&Previous->Mutex); Closed = Previous->Closing; }
            if (Closed.IsValid()) { Closed.Wait(); }
        }
        { FScopeLock Lock(&Request->Mutex); if (Request->bCancelled) { return false; } }
        auto Created = FPostHogClient::CreateAsync(MoveTemp(Config)).Get();
        FScopeLock Lock(&Request->Mutex);
        if (Request->bCancelled)
        { if (Created) { Request->Closing = Created->ShutdownAsync(); } return false; }
        Request->Client = MoveTemp(Created);
        return Request->Client.IsValid();
    });
    return true;
}

bool UPostHogSubsystem::IsInitialized() const
{ return IsInGameThread() && State == EPostHogSubsystemState::Ready && Client && Client->IsInitialized(); }
FPostHogClient::FPtr UPostHogSubsystem::GetClient() const
{ return IsInitialized() ? Client : nullptr; }

void UPostHogSubsystem::Shutdown()
{
    if (!IsInGameThread() || State == EPostHogSubsystemState::ShuttingDown || State == EPostHogSubsystemState::Deinitialized) { return; }
    StopReplay();
    StopObserving();
    if (Initialization)
    {
        auto Request = Initialization; Request->Cancel(); Initialization.Reset(); Client.Reset();
        Closing = PostHogTasks::Run([Request]
        {
            Request->Ready.Wait();
            TSharedFuture<bool> Closed;
            { FScopeLock Lock(&Request->Mutex); Closed = Request->Closing; }
            return !Closed.IsValid() || Closed.Get();
        });
    }
    else if (Client) { Closing = Client->ShutdownAsync(); Client.Reset(); }
    else { State = EPostHogSubsystemState::Idle; return; }
    State = EPostHogSubsystemState::ShuttingDown;
    EnsureTicker();
}

void UPostHogSubsystem::EnsureTicker()
{
    if (!TickerHandle.IsValid())
    {
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this](float DeltaTime) { return Tick(DeltaTime); }));
    }
}

bool UPostHogSubsystem::Tick(float DeltaTime)
{
    if (!IsInGameThread()) { return true; }
    if (Signals)
    {
        for (bool bNextForeground : Signals->Take())
        {
            if (bForeground == bNextForeground) { continue; }
            bForeground = bNextForeground;
            if (auto Current = Client)
            { if (bForeground) { Current->OnForeground(); } else { Current->OnBackground(); } }
            if (Replay) { Replay->SetPaused(!bForeground); }
        }
    }
    if (State == EPostHogSubsystemState::Initializing && Initialization && Initialization->Ready.IsReady())
    {
        const bool bSuccess = Initialization->Ready.Get();
        { FScopeLock Lock(&Initialization->Mutex); Client = Initialization->Client; }
        State = bSuccess && Client ? EPostHogSubsystemState::Ready : EPostHogSubsystemState::Failed;
        if (State == EPostHogSubsystemState::Ready)
        {
            auto Current = Client; Current->StartApplicationLifecycle(bForeground);
            if (State == EPostHogSubsystemState::Ready && Client == Current)
            {
                if (ReplaySettings.bAutoStart) { StartSessionReplay(); }
                OnSetupCompleted.Broadcast(true);
            }
        }
        else { Initialization->Cancel(); Initialization.Reset(); StopObserving(); OnSetupCompleted.Broadcast(false); }
    }
    if (State == EPostHogSubsystemState::ShuttingDown && Closing.IsReady())
    {
        const bool bSuccess = Closing.Get(); Closing = {};
        State = EPostHogSubsystemState::Idle;
        OnShutdownCompleted.Broadcast(bSuccess);
    }
    if (State == EPostHogSubsystemState::Idle || State == EPostHogSubsystemState::Failed || State == EPostHogSubsystemState::Deinitialized)
    { TickerHandle.Reset(); return false; }
    return true;
}

void UPostHogSubsystem::StartObserving()
{
    Signals = MakeShared<FPostHogLifecycleSignals, ESPMode::ThreadSafe>();
    auto Inbox = Signals;
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
    if (SlateHandle.IsValid() && FSlateApplication::IsInitialized())
    { FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(SlateHandle); }
    FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
    BackgroundHandle.Reset(); ForegroundHandle.Reset(); DeactivateHandle.Reset(); ReactivateHandle.Reset(); SlateHandle.Reset(); ExitHandle.Reset();
    Signals.Reset();
}

bool UPostHogSubsystem::StartSessionReplay()
{
    auto Current = GetClient();
    if (!Current || !ReplaySettings.bEnabled) { return false; }
    if (Replay) { return true; }
    Replay = FPostHogReplayRecorder::Start(Current, ReplaySettings);
    if (Replay) { Replay->SetPaused(!bForeground); }
    return Replay.IsValid();
}

void UPostHogSubsystem::StopReplay()
{
    if (Replay) { Replay->Stop(); Replay.Reset(); }
}

void UPostHogSubsystem::Stop()
{
    if (State == EPostHogSubsystemState::Deinitialized) { return; }
    StopReplay();
    Shutdown();
    StopObserving();
    FTSTicker::RemoveTicker(TickerHandle); TickerHandle.Reset();
    State = EPostHogSubsystemState::Deinitialized;
}
void UPostHogSubsystem::HandleEngineExit() { Stop(); }
void UPostHogSubsystem::Deinitialize() { Stop(); Super::Deinitialize(); }
