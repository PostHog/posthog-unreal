#include "PostHogSubsystem.h"
#include "PostHogAnalyticsProvider.h"
#include "PostHogJson.h"
#include "PostHogReplayRecorder.h"
#include "Async/Async.h"
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
#include <atomic>
#if !PLATFORM_EXCEPTIONS_DISABLED
#include <exception>
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPostHogSubsystem, Log, All);

struct FPostHogNamespaceLease
{
    FString Key;
    TSharedPtr<TPromise<void>, ESPMode::ThreadSafe> DonePromise;
    TSharedFuture<void> DoneFuture;
    FCriticalSection Mutex;
    bool bCancelled = false;
    bool bInitFinished = false;
    FPostHogClient::FPtr PendingCreated;
    std::atomic<bool> bCompleted{false};
    bool bReleasing = false;        // Game-thread only
    uint64 ShutdownGeneration = 0;  // Protected by Mutex

    void Complete()
    {
        if (!bCompleted.exchange(true) && DonePromise)
        {
            DonePromise->SetValue();
        }
    }
};

namespace
{
// Game-thread map of active or releasing namespace leases keyed by storage namespace,
// preventing concurrent owners and serializing replacement initialization until the
// previous client finishes both initialization and shutdown.
TMap<FString, TSharedRef<FPostHogNamespaceLease>> NamespaceLeases;

void ReleaseLeaseFromMap(const TSharedPtr<FPostHogNamespaceLease>& Lease)
{
    check(IsInGameThread());
    if (!Lease) { return; }
    if (auto* Current = NamespaceLeases.Find(Lease->Key); Current && &Current->Get() == Lease.Get())
    {
        NamespaceLeases.Remove(Lease->Key);
    }
}

FString MakeNamespaceKey(const FPostHogConfig& Config)
{
    return FPaths::ConvertRelativePathToFull(Config.StorageRoot) + TEXT("|")
        + Config.ProjectToken.TrimStartAndEnd() + TEXT("|") + Config.ClientName;
}

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
    {
        Context.SetString(TEXT("$app_version"), Version);
    }
    return Context;
}
}

void UPostHogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);
    const auto* Settings = GetDefault<UPostHogSettings>();
    if (Settings->bAutoInitialize && !Settings->Options.ProjectToken.IsEmpty())
    {
        Setup(Settings->Options);
    }
}

bool UPostHogSubsystem::Setup(const FPostHogOptions& Options)
{
    return SetupWithConfig(Options.ToNative(), Options.bEnableInEditor, Options.bEnableOnDedicatedServer);
}

bool UPostHogSubsystem::SetupWithConfig(FPostHogConfig Config, bool bEnableInEditor, bool bEnableOnDedicatedServer)
{
    if (!IsInGameThread() || (State != EPostHogSubsystemState::Idle && State != EPostHogSubsystemState::Failed))
    {
        return false;
    }
    auto* Instance = GetGameInstance();
    const auto* WorldContext = Instance ? Instance->GetWorldContext() : nullptr;
    if (!WorldContext || (WorldContext->WorldType != EWorldType::Game && WorldContext->WorldType != EWorldType::PIE) ||
        IsRunningCommandlet() || (GIsEditor && !bEnableInEditor) || (Instance->IsDedicatedServerInstance() && !bEnableOnDedicatedServer))
    {
        return false;
    }

    const FString Scope = WorldContext->WorldType == EWorldType::PIE
        ? FString::Printf(TEXT("editor/pie-%d"), WorldContext->PIEInstance)
        : (GIsEditor ? FString(TEXT("editor/standalone")) : FString(TEXT("runtime")));
    Config.ClientName = Scope + TEXT("/") + Config.ClientName;
    if (Config.StorageRoot.IsEmpty())
    {
        Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogStateful");
    }

    auto Context = SampleContext().ToNative();
    PostHogJson::Merge(*Context, *Config.Context.ToNative());
    Config.Context = FPostHogProperties::FromNative(Context);
    if (!Config.Context.IsValid()) { return false; }

    const FString Key = MakeNamespaceKey(Config);
    TSharedFuture<void> Predecessor;
    if (auto* Existing = NamespaceLeases.Find(Key))
    {
        if (!(*Existing)->bReleasing && !(*Existing)->DoneFuture.IsReady())
        {
            UE_LOG(LogPostHogSubsystem, Warning,
                TEXT("Rejecting PostHog setup: another active client already owns namespace '%s'."), *Key);
            return false;
        }
        if (!(*Existing)->DoneFuture.IsReady())
        {
            Predecessor = (*Existing)->DoneFuture;
        }
    }

    auto Lease = MakeShared<FPostHogNamespaceLease>();
    Lease->Key = Key;
    Lease->DonePromise = MakeShared<TPromise<void>, ESPMode::ThreadSafe>();
    Lease->DoneFuture = Lease->DonePromise->GetFuture().Share();
    NamespaceLeases.Add(Key, Lease);

    NamespaceKey = Key;
    ActiveLease = Lease;
    PendingFlagUpdates.Empty();
    ReplaySettings = Config.Replay;
    bCaptureApplicationLifecycleEvents = Config.bCaptureApplicationLifecycleEvents;
    State = EPostHogSubsystemState::Initializing;
    const uint64 Generation = ++SetupGeneration;
    StartObserving();

    TWeakObjectPtr<UPostHogSubsystem> WeakThis(this);
    auto UserFlagCallback = MoveTemp(Config.OnFeatureFlagsChanged);
    Config.OnFeatureFlagsChanged = [WeakThis, Generation, UserFlagCallback = MoveTemp(UserFlagCallback)](FPostHogFeatureFlagsUpdate Update)
    {
        if (UserFlagCallback)
        {
#if PLATFORM_EXCEPTIONS_DISABLED
            UserFlagCallback(Update);
#else
            try
            {
                UserFlagCallback(Update);
            }
            catch (const std::exception&)
            {
                UE_LOG(LogPostHogSubsystem, Warning, TEXT("OnFeatureFlagsChanged threw std::exception."));
            }
            catch (...)
            {
                UE_LOG(LogPostHogSubsystem, Warning, TEXT("OnFeatureFlagsChanged threw an unknown exception."));
            }
#endif
        }
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Generation, Update = MoveTemp(Update)]() mutable
        {
            auto* Self = WeakThis.Get();
            if (!Self || Self->SetupGeneration != Generation) { return; }
            if (Self->State == EPostHogSubsystemState::Initializing)
            {
                Self->PendingFlagUpdates.Add(MoveTemp(Update));
                return;
            }
            if (Self->State == EPostHogSubsystemState::Ready)
            {
                TStrongObjectPtr<UPostHogSubsystem> KeepAlive(Self);
                Self->OnFeatureFlagsChanged.Broadcast(Update);
            }
        });
    };

    auto InitTask = [WeakThis, Generation, Lease, Predecessor, Config = MoveTemp(Config)]() mutable
    {
        if (Predecessor.IsValid()) { Predecessor.Wait(); }
        {
            FScopeLock Lock(&Lease->Mutex);
            if (Lease->bCancelled)
            {
                const uint64 ShutdownGen = Lease->ShutdownGeneration;
                Lease->Complete();
                AsyncTask(ENamedThreads::GameThread, [WeakThis, Lease, ShutdownGen]()
                {
                    ReleaseLeaseFromMap(Lease);
                    if (auto* Self = WeakThis.Get()) { Self->FinishShutdown(Lease, ShutdownGen, FPostHogDrainResult{}); }
                });
                return;
            }
        }

        auto Created = FPostHogClient::CreateSync(MoveTemp(Config));
        bool bCancelledDuringInit = false;
        uint64 ShutdownGen = 0;
        {
            FScopeLock Lock(&Lease->Mutex);
            if (Lease->bCancelled)
            {
                bCancelledDuringInit = true;
                ShutdownGen = Lease->ShutdownGeneration;
            }
            else
            {
                Lease->PendingCreated = MoveTemp(Created);
                Lease->bInitFinished = true;
            }
        }
        if (bCancelledDuringInit)
        {
            FPostHogDrainResult Result;
            if (Created)
            {
                Result = Created->ShutdownSync();
                Created.Reset();
            }
            Lease->Complete();
            AsyncTask(ENamedThreads::GameThread, [WeakThis, Lease, ShutdownGen, Result]()
            {
                ReleaseLeaseFromMap(Lease);
                if (auto* Self = WeakThis.Get()) { Self->FinishShutdown(Lease, ShutdownGen, Result); }
            });
            return;
        }

        AsyncTask(ENamedThreads::GameThread, [WeakThis, Generation, Lease]() mutable
        {
            FPostHogClient::FPtr Created;
            bool bCancelled = false;
            {
                FScopeLock Lock(&Lease->Mutex);
                bCancelled = Lease->bCancelled;
                Created = MoveTemp(Lease->PendingCreated);
            }
            if (bCancelled)
            {
                // ShutdownInternal already took ownership of PendingCreated or completed the lease.
                return;
            }
            auto* Self = WeakThis.Get();
            if (!Self || Self->SetupGeneration != Generation || Self->State != EPostHogSubsystemState::Initializing)
            {
                if (Created)
                {
                    auto Cleanup = [Lease, Created = MoveTemp(Created)]() mutable
                    {
                        Created->ShutdownSync();
                        Created.Reset();
                        Lease->Complete();
                        AsyncTask(ENamedThreads::GameThread, [Lease]() { ReleaseLeaseFromMap(Lease); });
                    };
                    if (!FPostHogClient::LaunchTrackedTask(MoveTemp(Cleanup)))
                    {
                        Created->ShutdownSync();
                        Created.Reset();
                        Lease->Complete();
                        ReleaseLeaseFromMap(Lease);
                    }
                }
                else
                {
                    Lease->Complete();
                    ReleaseLeaseFromMap(Lease);
                }
                return;
            }
            TStrongObjectPtr<UPostHogSubsystem> KeepAlive(Self);
            if (!Created)
            {
                Lease->bReleasing = true;
                Lease->Complete();
                ReleaseLeaseFromMap(Lease);
                if (Self->ActiveLease == Lease) { Self->ActiveLease.Reset(); }
                Self->PendingFlagUpdates.Empty();
                Self->State = EPostHogSubsystemState::Failed;
                Self->StopObserving();
                Self->OnSetupCompleted.Broadcast(false);
                return;
            }
            Self->Client = MoveTemp(Created);
            Self->State = EPostHogSubsystemState::Ready;
            Self->Client->SetAppInBackground(!Self->bForeground);
            if (!Self->Client->IsDisabled() && Self->bCaptureApplicationLifecycleEvents && Self->bForeground)
            {
                FPostHogProperties Props;
                Props.SetBoolean(TEXT("from_background"), false);
                Self->Client->Capture(TEXT("Application Opened"), Props);
            }
            if (Self->SetupGeneration == Generation && Self->State == EPostHogSubsystemState::Ready)
            {
                Self->OnSetupCompleted.Broadcast(true);
            }
            auto BufferedUpdates = MoveTemp(Self->PendingFlagUpdates);
            Self->PendingFlagUpdates.Empty();
            for (const FPostHogFeatureFlagsUpdate& Update : BufferedUpdates)
            {
                if (Self->SetupGeneration != Generation || Self->State != EPostHogSubsystemState::Ready)
                {
                    break;
                }
                Self->OnFeatureFlagsChanged.Broadcast(Update);
            }
        });
    };

    if (!FPostHogClient::LaunchTrackedTask(MoveTemp(InitTask)))
    {
        Lease->bReleasing = true;
        Lease->Complete();
        ReleaseLeaseFromMap(Lease);
        ActiveLease.Reset();
        PendingFlagUpdates.Empty();
        State = EPostHogSubsystemState::Failed;
        StopObserving();
        return false;
    }
    return true;
}

bool UPostHogSubsystem::IsInitialized() const
{
    return IsInGameThread() && State == EPostHogSubsystemState::Ready && Client && Client->IsInitialized();
}

bool UPostHogSubsystem::IsDisabled() const
{
    auto Current = GetClient();
    return !Current || Current->IsDisabled();
}

FPostHogClient::FPtr UPostHogSubsystem::GetClient() const { return IsInitialized() ? Client : nullptr; }

EPostHogCaptureResult UPostHogSubsystem::Capture(const FString& Event, const FPostHogProperties& Properties)
{
    auto Current = GetClient();
    return Current ? Current->Capture(Event, Properties) : EPostHogCaptureResult::Closed;
}

void UPostHogSubsystem::Identify(const FString& DistinctId, const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (auto Current = GetClient()) { Current->Identify(DistinctId, Set, SetOnce); }
}

void UPostHogSubsystem::Reset() { if (auto Current = GetClient()) { Current->Reset(); } }
void UPostHogSubsystem::OptIn() { if (auto Current = GetClient()) { Current->OptIn(); } }

void UPostHogSubsystem::OptOut()
{
    auto Current = GetClient();
    if (!Current) { return; }
    Current->OptOut();
    StopSessionReplay();
}

FPostHogFeatureFlagResult UPostHogSubsystem::GetFeatureFlag(const FString& Key, EPostHogFlagReporting Reporting)
{
    auto Current = GetClient();
    return Current ? Current->GetFeatureFlagResult(Key, Reporting) : FPostHogFeatureFlagResult{};
}

FPostHogFeatureFlags UPostHogSubsystem::GetFeatureFlags() const
{
    auto Current = GetClient();
    return Current ? Current->GetFeatureFlags() : FPostHogFeatureFlags{};
}

void UPostHogSubsystem::ReloadFeatureFlags() { if (auto Current = GetClient()) { Current->ReloadFeatureFlags(); } }

void UPostHogSubsystem::StartSessionReplay()
{
    auto Current = GetClient();
    if (!Current || Current->IsDisabled() || Current->IsOptedOut()) { return; }
    if (!Recorder)
    {
        Recorder = FPostHogReplayRecorder::Attach(GetGameInstance(), Current, ReplaySettings);
        if (!Recorder) { return; }
        Recorder->SetAvailable(bForeground);
    }
    Current->StartSessionReplay();
}

void UPostHogSubsystem::StopSessionReplay()
{
    auto Current = GetClient();
    if (!Current) { return; }
    Current->StopSessionReplay();
    if (Recorder)
    {
        Recorder->Stop();
        Recorder.Reset();
    }
}

bool UPostHogSubsystem::IsSessionReplayActive() const
{
    auto Current = GetClient();
    return Current && Recorder && Current->IsSessionReplayActive();
}

TSharedPtr<IAnalyticsProvider> UPostHogSubsystem::GetAnalyticsProvider()
{
    auto Current = GetClient();
    if (!Current) { return nullptr; }
    if (!AnalyticsProvider) { AnalyticsProvider = MakeShareable(new FPostHogAnalyticsProvider(Current)); }
    return AnalyticsProvider;
}

bool UPostHogSubsystem::BindAnalyticsProvider()
{
    return GetAnalyticsProvider() && FPostHogAnalyticsProvider::Bind(AnalyticsProvider);
}

void UPostHogSubsystem::UnbindAnalyticsProvider()
{
    if (IsInGameThread() && AnalyticsProvider) { AnalyticsProvider->Unbind(); }
}

void UPostHogSubsystem::Shutdown()
{
    ShutdownInternal(false);
}

void UPostHogSubsystem::ShutdownInternal(bool bDeinitializing)
{
    if (!IsInGameThread() || State == EPostHogSubsystemState::ShuttingDown || State == EPostHogSubsystemState::Deinitialized)
    {
        return;
    }
    const EPostHogSubsystemState PreviousState = State;
    ++SetupGeneration;
    PendingFlagUpdates.Empty();
    UnbindAnalyticsProvider();
    AnalyticsProvider.Reset();
    StopObserving();
    if (Recorder)
    {
        Recorder->Stop();
        Recorder.Reset();
    }
    auto ActiveClient = MoveTemp(Client);
    Client.Reset();
    auto Lease = ActiveLease;

    if (PreviousState == EPostHogSubsystemState::Initializing && Lease.IsValid())
    {
        Lease->bReleasing = true;
        FPostHogClient::FPtr Pending;
        bool bAlreadyFinishedInit = false;
        {
            FScopeLock Lock(&Lease->Mutex);
            Lease->bCancelled = true;
            Lease->ShutdownGeneration = SetupGeneration;
            bAlreadyFinishedInit = Lease->bInitFinished;
            Pending = MoveTemp(Lease->PendingCreated);
        }
        if (!bAlreadyFinishedInit)
        {
            State = EPostHogSubsystemState::ShuttingDown;
            return;
        }
        if (!Pending)
        {
            Lease->Complete();
            ReleaseLeaseFromMap(Lease);
            ActiveLease.Reset();
            State = EPostHogSubsystemState::Idle;
            if (!bDeinitializing)
            {
                TStrongObjectPtr<UPostHogSubsystem> KeepAlive(this);
                OnShutdownCompleted.Broadcast(FPostHogDrainResult{});
            }
            return;
        }
        ActiveClient = MoveTemp(Pending);
    }

    if (!ActiveClient)
    {
        if (Lease)
        {
            Lease->bReleasing = true;
            Lease->Complete();
            ReleaseLeaseFromMap(Lease);
            ActiveLease.Reset();
        }
        State = EPostHogSubsystemState::Idle;
        return;
    }

    if (Lease) { Lease->bReleasing = true; }
    State = EPostHogSubsystemState::ShuttingDown;

    TWeakObjectPtr<UPostHogSubsystem> WeakThis(this);
    const uint64 Generation = SetupGeneration;
    auto ShutdownTask = [WeakThis, Generation, Lease, ActiveClient = MoveTemp(ActiveClient)]() mutable
    {
        const FPostHogDrainResult Result = ActiveClient->ShutdownSync();
        ActiveClient.Reset();
        if (Lease) { Lease->Complete(); }
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Generation, Lease, Result]()
        {
            ReleaseLeaseFromMap(Lease);
            if (auto* Self = WeakThis.Get()) { Self->FinishShutdown(Lease, Generation, Result); }
        });
    };

    if (!FPostHogClient::LaunchTrackedTask(MoveTemp(ShutdownTask)))
    {
        // Module stopping: run synchronously so the namespace lease resolves cleanly.
        const FPostHogDrainResult Result = ActiveClient->ShutdownSync();
        ActiveClient.Reset();
        if (Lease)
        {
            Lease->Complete();
            ReleaseLeaseFromMap(Lease);
        }
        ActiveLease.Reset();
        State = EPostHogSubsystemState::Idle;
        if (!bDeinitializing)
        {
            TStrongObjectPtr<UPostHogSubsystem> KeepAlive(this);
            OnShutdownCompleted.Broadcast(Result);
        }
    }
}

void UPostHogSubsystem::FinishShutdown(TSharedPtr<FPostHogNamespaceLease> Lease, uint64 Generation, const FPostHogDrainResult& Result)
{
    check(IsInGameThread());
    if (SetupGeneration != Generation || State != EPostHogSubsystemState::ShuttingDown)
    {
        return;
    }
    TStrongObjectPtr<UPostHogSubsystem> KeepAlive(this);
    if (ActiveLease == Lease) { ActiveLease.Reset(); }
    State = EPostHogSubsystemState::Idle;
    OnShutdownCompleted.Broadcast(Result);
}

void UPostHogSubsystem::HandleLifecycleChange(bool bNextForeground)
{
    if (!IsInGameThread())
    {
        TWeakObjectPtr<UPostHogSubsystem> WeakThis(this);
        AsyncTask(ENamedThreads::GameThread, [WeakThis, bNextForeground]()
        {
            if (auto* Self = WeakThis.Get()) { Self->HandleLifecycleChange(bNextForeground); }
        });
        return;
    }
    if (bForeground == bNextForeground) { return; }
    bForeground = bNextForeground;
    auto Current = GetClient();
    if (!Current) { return; }
    TStrongObjectPtr<UPostHogSubsystem> KeepAlive(this);
    if (Recorder) { Recorder->SetAvailable(bForeground); }
    if (bForeground) { Current->SetAppInBackground(false); }
    if (GetClient() != Current) { return; }
    if (!Current->IsDisabled() && bCaptureApplicationLifecycleEvents)
    {
        FPostHogProperties Properties;
        if (bForeground) { Properties.SetBoolean(TEXT("from_background"), true); }
        Current->Capture(bForeground ? TEXT("Application Opened") : TEXT("Application Backgrounded"), Properties);
        if (GetClient() != Current) { return; }
    }
    if (!bForeground) { Current->SetAppInBackground(true); }
}

void UPostHogSubsystem::StartObserving()
{
    TWeakObjectPtr<UPostHogSubsystem> WeakThis(this);
    auto Forward = [WeakThis](bool bNext)
    {
        if (!IsInGameThread())
        {
            AsyncTask(ENamedThreads::GameThread, [WeakThis, bNext]()
            {
                if (auto* Self = WeakThis.Get()) { Self->HandleLifecycleChange(bNext); }
            });
            return;
        }
        if (auto* Self = WeakThis.Get()) { Self->HandleLifecycleChange(bNext); }
    };
    BackgroundHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda([Forward] { Forward(false); });
    ForegroundHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddLambda([Forward] { Forward(true); });
    DeactivateHandle = FCoreDelegates::ApplicationWillDeactivateDelegate.AddLambda([Forward] { Forward(false); });
    ReactivateHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddLambda([Forward] { Forward(true); });
    if (FSlateApplication::IsInitialized())
    {
        bForeground = FSlateApplication::Get().IsActive();
        SlateHandle = FSlateApplication::Get().OnApplicationActivationStateChanged().AddLambda([Forward](bool bActive) { Forward(bActive); });
    }
    else
    {
        bForeground = true;
    }
    ExitHandle = FCoreDelegates::OnEnginePreExit.AddUObject(this, &UPostHogSubsystem::Stop);
}

void UPostHogSubsystem::StopObserving()
{
    FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(BackgroundHandle);
    FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(ForegroundHandle);
    FCoreDelegates::ApplicationWillDeactivateDelegate.Remove(DeactivateHandle);
    FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(ReactivateHandle);
    if (SlateHandle.IsValid() && FSlateApplication::IsInitialized())
    {
        FSlateApplication::Get().OnApplicationActivationStateChanged().Remove(SlateHandle);
    }
    FCoreDelegates::OnEnginePreExit.Remove(ExitHandle);
    BackgroundHandle.Reset();
    ForegroundHandle.Reset();
    DeactivateHandle.Reset();
    ReactivateHandle.Reset();
    SlateHandle.Reset();
    ExitHandle.Reset();
}

void UPostHogSubsystem::Stop()
{
    if (State == EPostHogSubsystemState::Deinitialized) { return; }
    ShutdownInternal(true);
    State = EPostHogSubsystemState::Deinitialized;
}

void UPostHogSubsystem::Deinitialize()
{
    Stop();
    Super::Deinitialize();
}
