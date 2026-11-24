#include "PostHogClient.h"
#include "PostHogJson.h"
#include "PostHogReplayFrame.h"
#include "Async/Async.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/Optional.h"
#include "Misc/ScopeLock.h"
#include "posthog_stateful.h"
#include <atomic>
#if !PLATFORM_EXCEPTIONS_DISABLED
#include <exception>
#endif
#include <type_traits>

DEFINE_LOG_CATEGORY_STATIC(LogPostHog, Log, All);

namespace
{
constexpr int32 MaxFlagJsonBytes = 4 * 1024 * 1024;
constexpr int32 MaxHookJsonBytes = 32 * 1024 * 1024;

// Track live clients and in-flight async tasks so module shutdown can drain cleanly.
// All fields below are protected exclusively by RegistryMutex.
FCriticalSection RegistryMutex;
int32 InFlightTasks = 0;
FEvent* TasksIdleEvent = nullptr;
bool bModuleStopping = false;

bool TryTrackTaskStart()
{
    FScopeLock Lock(&RegistryMutex);
    if (bModuleStopping) { return false; }
    if (!TasksIdleEvent) { TasksIdleEvent = FPlatformProcess::GetSynchEventFromPool(true); }
    if (InFlightTasks++ == 0) { TasksIdleEvent->Reset(); }
    return true;
}

void TrackTaskEnd()
{
    FScopeLock Lock(&RegistryMutex);
    if (--InFlightTasks == 0 && TasksIdleEvent) { TasksIdleEvent->Trigger(); }
}

template <typename T>
TSharedFuture<T> ReadyFuture(T Value)
{
    TPromise<T> Promise;
    auto Future = Promise.GetFuture().Share();
    Promise.SetValue(MoveTemp(Value));
    return Future;
}

template <typename T, typename Fn>
TSharedFuture<T> RunAsync(Fn&& Work, T StoppedValue = T())
{
    if (!TryTrackTaskStart()) { return ReadyFuture(MoveTemp(StoppedValue)); }
    auto Promise = MakeShared<TPromise<T>, ESPMode::ThreadSafe>();
    auto Future = Promise->GetFuture().Share();
    Async(EAsyncExecution::ThreadPool,
        [Promise, OptWork = TOptional<std::decay_t<Fn>>(MoveTemp(Work))]() mutable
        {
            T Result;
            {
                auto LocalWork = MoveTemp(*OptWork);
                OptWork.Reset();
                Result = LocalWork();
            }
            // LocalWork (and any captured TSharedPtr<FImpl>) is destroyed before resolving
            // Promise and decrementing InFlightTasks.
            Promise->SetValue(MoveTemp(Result));
            TrackTaskEnd();
        });
    return Future;
}

void LogConfigErrorIfFailed(const TCHAR* Field, PostHogStatus Status)
{
    if (Status != POSTHOG_STATUS_OK)
    {
        const char* Err = posthog_last_error();
        UE_LOG(LogPostHog, Warning, TEXT("PostHog config '%s' rejected (status %d): %s"),
            Field, static_cast<int32>(Status), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
    }
}

FString TakeOwnedString(char* Raw)
{
    if (!Raw) { return FString(); }
    FString Out = UTF8_TO_TCHAR(Raw);
    posthog_string_free(Raw);
    return Out;
}

EPostHogCaptureResult ToCaptureResult(PostHogCaptureOutcome Outcome)
{
    switch (Outcome)
    {
    case POSTHOG_CAPTURE_ACCEPTED: return EPostHogCaptureResult::Accepted;
    case POSTHOG_CAPTURE_DISABLED: return EPostHogCaptureResult::Disabled;
    case POSTHOG_CAPTURE_OPTED_OUT: return EPostHogCaptureResult::OptedOut;
    case POSTHOG_CAPTURE_CLOSED: return EPostHogCaptureResult::Closed;
    case POSTHOG_CAPTURE_DROPPED: return EPostHogCaptureResult::Dropped;
    case POSTHOG_CAPTURE_REENTRANT: return EPostHogCaptureResult::Reentrant;
    default: return EPostHogCaptureResult::Invalid;
    }
}

EPostHogPersistenceOutcome ToPersistenceOutcome(PostHogPersistenceOutcome Outcome)
{
    switch (Outcome)
    {
    case POSTHOG_PERSISTENCE_PERSISTED: return EPostHogPersistenceOutcome::Persisted;
    case POSTHOG_PERSISTENCE_MEMORY_ONLY: return EPostHogPersistenceOutcome::MemoryOnly;
    case POSTHOG_PERSISTENCE_UNAVAILABLE: return EPostHogPersistenceOutcome::Unavailable;
    case POSTHOG_PERSISTENCE_WRITE_FAILED: return EPostHogPersistenceOutcome::WriteFailed;
    case POSTHOG_PERSISTENCE_TIMED_OUT: return EPostHogPersistenceOutcome::TimedOut;
    case POSTHOG_PERSISTENCE_DEFERRED: return EPostHogPersistenceOutcome::Deferred;
    case POSTHOG_PERSISTENCE_DISABLED: return EPostHogPersistenceOutcome::Disabled;
    case POSTHOG_PERSISTENCE_NOT_REQUESTED: return EPostHogPersistenceOutcome::NotRequested;
    default: return EPostHogPersistenceOutcome::Unknown;
    }
}

EPostHogDeliveryOutcome ToDeliveryOutcome(PostHogDeliveryOutcome Outcome)
{
    switch (Outcome)
    {
    case POSTHOG_DELIVERY_ATTEMPTED: return EPostHogDeliveryOutcome::Attempted;
    case POSTHOG_DELIVERY_STOPPED: return EPostHogDeliveryOutcome::Stopped;
    case POSTHOG_DELIVERY_TIMED_OUT: return EPostHogDeliveryOutcome::TimedOut;
    case POSTHOG_DELIVERY_UNAVAILABLE: return EPostHogDeliveryOutcome::Unavailable;
    case POSTHOG_DELIVERY_CLOSED: return EPostHogDeliveryOutcome::Closed;
    case POSTHOG_DELIVERY_DEFERRED: return EPostHogDeliveryOutcome::Deferred;
    case POSTHOG_DELIVERY_DISABLED: return EPostHogDeliveryOutcome::Disabled;
    default: return EPostHogDeliveryOutcome::Unknown;
    }
}

FPostHogDrainResult ToDrainResult(const PostHogDrainReport& Report)
{
    FPostHogDrainResult Out;
    Out.State = ToPersistenceOutcome(Report.state);
    Out.Delivery = ToDeliveryOutcome(Report.delivery);
    return Out;
}

PostHogFlagReporting ToNativeReporting(EPostHogFlagReporting Reporting)
{
    switch (Reporting)
    {
    case EPostHogFlagReporting::Send: return POSTHOG_FLAG_REPORTING_SEND;
    case EPostHogFlagReporting::Silent: return POSTHOG_FLAG_REPORTING_SILENT;
    default: return POSTHOG_FLAG_REPORTING_DEFAULT;
    }
}

FPostHogFeatureFlagResult ParseFlagResult(const TSharedPtr<FJsonValue>& Value)
{
    FPostHogFeatureFlagResult Out;
    if (!Value.IsValid() || Value->Type != EJson::Object) { return Out; }
    const auto Object = Value->AsObject();
    const auto Variant = Object->TryGetField(TEXT("variant"));
    const auto Payload = Object->TryGetField(TEXT("payload"));
    if (!Object->TryGetBoolField(TEXT("enabled"), Out.bEnabled) ||
        !Object->TryGetBoolField(TEXT("has_payload"), Out.bHasPayload) ||
        !Variant.IsValid() || !Payload.IsValid())
    {
        return Out;
    }
    Out.Value = Variant->Type == EJson::String
        ? FPostHogValue::String(Variant->AsString())
        : FPostHogValue::Boolean(Out.bEnabled);
    Out.Record = FPostHogProperties::FromNative(Object.ToSharedRef(), MaxFlagJsonBytes);
    FPostHogValue::TryParse(PostHogJson::EncodeValue(Payload), Out.Payload, MaxFlagJsonBytes);
    Out.bFound = true;
    return Out;
}

FPostHogFeatureFlags ParseFlagMap(const TSharedPtr<FJsonValue>& Value)
{
    FPostHogFeatureFlags Out;
    if (!Value.IsValid() || Value->Type != EJson::Object) { return Out; }
    for (const auto& Pair : Value->AsObject()->Values)
    {
        auto Record = ParseFlagResult(Pair.Value);
        if (Record.bFound) { Out.Flags.Add(FString(Pair.Key), MoveTemp(Record)); }
    }
    Out.bAvailable = true;
    return Out;
}

FPostHogFeatureFlagsUpdate ParseFlagsUpdate(const FString& Json)
{
    FPostHogFeatureFlagsUpdate Out;
    const auto Object = PostHogJson::Decode(Json, MaxFlagJsonBytes);
    if (!Object.IsValid()) { return Out; }
    Object->TryGetBoolField(TEXT("errors_loading"), Out.bErrorsLoading);
    Object->TryGetBoolField(TEXT("partial"), Out.bPartial);
    Object->TryGetBoolField(TEXT("invalid_payloads"), Out.bInvalidPayloads);
    const TSharedPtr<FJsonObject>* Context = nullptr;
    if (Object->TryGetObjectField(TEXT("context"), Context) && Context)
    {
        Out.Context = FPostHogProperties::FromNative(Context->ToSharedRef(), MaxFlagJsonBytes);
    }
    if (const auto Error = Object->TryGetField(TEXT("error")))
    {
        FPostHogValue::TryParse(PostHogJson::EncodeValue(Error), Out.Error, MaxFlagJsonBytes);
    }
    Out.Snapshot = ParseFlagMap(Object->TryGetField(TEXT("flags")));
    return Out;
}
}

struct FPostHogClient::FImpl : TSharedFromThis<FImpl, ESPMode::ThreadSafe>
{
    PostHogStatefulClient* Native = nullptr;
    std::atomic<bool> bClosed{false};
    std::atomic<bool> bForeground{true};
    TFunction<bool(FJsonObject&)> BeforeSend;
    TFunction<void(FPostHogFeatureFlagsUpdate)> OnFeatureFlags;

    ~FImpl()
    {
        if (Native)
        {
            posthog_stateful_client_free(Native);
            Native = nullptr;
        }
    }

    static bool HandleBeforeSend(void* UserData, const char* EventJson, char** OutEventJson)
    {
        auto& Self = *static_cast<FImpl*>(UserData);
        if (!EventJson || !Self.BeforeSend) { return false; }
        auto Event = PostHogJson::Decode(UTF8_TO_TCHAR(EventJson), MaxHookJsonBytes);
        if (!Event.IsValid()) { return false; }
        FString EventName;
        Event->TryGetStringField(TEXT("event"), EventName);
        bool bKeep = false;
#if PLATFORM_EXCEPTIONS_DISABLED
        bKeep = Self.BeforeSend(*Event);
#else
        try
        {
            bKeep = Self.BeforeSend(*Event);
        }
        catch (const std::exception&)
        {
            UE_LOG(LogPostHog, Warning, TEXT("Dropping event '%s': BeforeSend threw std::exception."), *EventName);
            return false;
        }
        catch (...)
        {
            UE_LOG(LogPostHog, Warning, TEXT("Dropping event '%s': BeforeSend threw an unknown exception."), *EventName);
            return false;
        }
#endif
        if (!bKeep) { return false; }
        const FString Modified = PostHogJson::Encode(Event.ToSharedRef());
        if (!PostHogJson::IsBounded(Modified, MaxHookJsonBytes))
        {
            UE_LOG(LogPostHog, Warning, TEXT("Dropping event '%s': BeforeSend output exceeded maximum JSON size (%d bytes)."),
                *EventName, MaxHookJsonBytes);
            return false;
        }
        *OutEventJson = posthog_string_copy(TCHAR_TO_UTF8(*Modified));
        return *OutEventJson != nullptr;
    }

    static void HandleFeatureFlags(void* UserData, const char* UpdateJson)
    {
        auto& Self = *static_cast<FImpl*>(UserData);
        if (!UpdateJson || !Self.OnFeatureFlags) { return; }
        auto Update = ParseFlagsUpdate(UTF8_TO_TCHAR(UpdateJson));
#if PLATFORM_EXCEPTIONS_DISABLED
        Self.OnFeatureFlags(MoveTemp(Update));
#else
        try
        {
            Self.OnFeatureFlags(MoveTemp(Update));
        }
        catch (const std::exception&)
        {
            UE_LOG(LogPostHog, Warning, TEXT("OnFeatureFlagsChanged threw std::exception."));
        }
        catch (...)
        {
            UE_LOG(LogPostHog, Warning, TEXT("OnFeatureFlagsChanged threw an unknown exception."));
        }
#endif
    }

    static void HandleError(void*, const char* Message)
    {
        if (Message) { UE_LOG(LogPostHog, Warning, TEXT("%s"), UTF8_TO_TCHAR(Message)); }
    }
};

TArray<TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe>> FPostHogClient::LiveClients;

bool FPostHogClient::LaunchTrackedTask(TFunction<void()>&& Task)
{
    if (!Task || !TryTrackTaskStart()) { return false; }
    Async(EAsyncExecution::ThreadPool,
        [OptTask = TOptional<TFunction<void()>>(MoveTemp(Task))]() mutable
        {
            {
                auto LocalTask = MoveTemp(*OptTask);
                OptTask.Reset();
                LocalTask();
            }
            TrackTaskEnd();
        });
    return true;
}

FPostHogClient::FPtr FPostHogClient::CreateSync(FPostHogConfig Config)
{
    {
        FScopeLock Lock(&RegistryMutex);
        if (bModuleStopping) { return nullptr; }
    }
    if (!Config.Context.IsValid()) { return nullptr; }
    auto State = MakeShared<FImpl, ESPMode::ThreadSafe>();
    State->BeforeSend = MoveTemp(Config.BeforeSend);
    State->OnFeatureFlags = MoveTemp(Config.OnFeatureFlagsChanged);

    PostHogStatefulConfig* NativeCfg = posthog_stateful_config_new(TCHAR_TO_UTF8(*Config.ProjectToken));
    if (!NativeCfg)
    {
        const char* Err = posthog_last_error();
        UE_LOG(LogPostHog, Error, TEXT("PostHog config creation failed: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
        return nullptr;
    }

    LogConfigErrorIfFailed(TEXT("host"), posthog_stateful_config_set_host(NativeCfg, TCHAR_TO_UTF8(*Config.Host)));
    LogConfigErrorIfFailed(TEXT("storage_dir"), posthog_stateful_config_set_storage_dir(
        NativeCfg, Config.StorageRoot.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Config.StorageRoot)));
    LogConfigErrorIfFailed(TEXT("instance_name"), posthog_stateful_config_set_instance_name(NativeCfg, TCHAR_TO_UTF8(*Config.ClientName)));
    LogConfigErrorIfFailed(TEXT("opt_out"), posthog_stateful_config_set_opt_out(NativeCfg, Config.bInitiallyOptedOut));
    LogConfigErrorIfFailed(TEXT("person_profiles"), posthog_stateful_config_set_person_profiles(NativeCfg,
        Config.PersonProfiles == EPostHogPersonProfiles::Never ? POSTHOG_PERSON_PROFILES_NEVER :
        Config.PersonProfiles == EPostHogPersonProfiles::Always ? POSTHOG_PERSON_PROFILES_ALWAYS :
        POSTHOG_PERSON_PROFILES_IDENTIFIED_ONLY));
    LogConfigErrorIfFailed(TEXT("session_mode"), posthog_stateful_config_set_session_mode(NativeCfg,
        Config.SessionMode == EPostHogSessionMode::Manual ? POSTHOG_SESSION_MODE_MANUAL : POSTHOG_SESSION_MODE_AUTOMATIC));
    LogConfigErrorIfFailed(TEXT("persist_sessions"), posthog_stateful_config_set_persist_sessions(NativeCfg, Config.bPersistSessions));
    LogConfigErrorIfFailed(TEXT("preload_feature_flags"), posthog_stateful_config_set_preload_feature_flags(NativeCfg, Config.bPreloadFeatureFlags));
    LogConfigErrorIfFailed(TEXT("send_feature_flag_event"), posthog_stateful_config_set_send_feature_flag_event(NativeCfg, Config.bSendFeatureFlagEvents));
    LogConfigErrorIfFailed(TEXT("platform_context_json"), posthog_stateful_config_set_platform_context_json(NativeCfg, TCHAR_TO_UTF8(*Config.Context.ToJson())));
    LogConfigErrorIfFailed(TEXT("flush_at"), posthog_stateful_config_set_flush_at(NativeCfg, static_cast<size_t>(FMath::Max(0, Config.FlushAt))));
    LogConfigErrorIfFailed(TEXT("flush_interval_ms"), posthog_stateful_config_set_flush_interval_ms(NativeCfg, static_cast<uint64>(FMath::Max(0, Config.FlushIntervalMilliseconds))));
    LogConfigErrorIfFailed(TEXT("max_queue_size"), posthog_stateful_config_set_max_queue_size(NativeCfg, static_cast<size_t>(FMath::Max(0, Config.MaxQueueSize))));
    LogConfigErrorIfFailed(TEXT("max_batch_size"), posthog_stateful_config_set_max_batch_size(NativeCfg, static_cast<size_t>(FMath::Max(0, Config.MaxBatchSize))));
    LogConfigErrorIfFailed(TEXT("request_timeout_seconds"), posthog_stateful_config_set_request_timeout_seconds(NativeCfg, static_cast<uint64>(FMath::Max(0, Config.RequestTimeoutSeconds))));
    LogConfigErrorIfFailed(TEXT("shutdown_timeout_ms"), posthog_stateful_config_set_shutdown_timeout_ms(NativeCfg, static_cast<uint64>(FMath::Max(0, Config.ShutdownTimeoutMilliseconds))));
    LogConfigErrorIfFailed(TEXT("replay_jpeg_quality"), posthog_stateful_config_set_replay_jpeg_quality(NativeCfg, static_cast<uint8>(FMath::Clamp(Config.Replay.JpegQuality, 1, 100))));
    LogConfigErrorIfFailed(TEXT("replay_max_frames_in_flight"), posthog_stateful_config_set_replay_max_frames_in_flight(NativeCfg, static_cast<size_t>(FMath::Max(0, Config.Replay.MaxFramesInFlight))));
    LogConfigErrorIfFailed(TEXT("replay_throttle_delay_ms"), posthog_stateful_config_set_replay_throttle_delay_ms(NativeCfg, static_cast<uint64>(FMath::Max(0, Config.Replay.IntervalMs))));
    LogConfigErrorIfFailed(TEXT("sdk_identity"), posthog_stateful_config_set_sdk_identity(NativeCfg, "posthog-unreal", "0.0.1"));
    LogConfigErrorIfFailed(TEXT("on_error"), posthog_stateful_config_set_on_error(NativeCfg, &FImpl::HandleError, nullptr));
    if (State->BeforeSend)
    {
        LogConfigErrorIfFailed(TEXT("before_send"), posthog_stateful_config_set_before_send(NativeCfg, &FImpl::HandleBeforeSend, &State.Get()));
    }
    if (State->OnFeatureFlags)
    {
        LogConfigErrorIfFailed(TEXT("on_feature_flags"), posthog_stateful_config_set_on_feature_flags(NativeCfg, &FImpl::HandleFeatureFlags, &State.Get()));
    }

    State->Native = posthog_stateful_client_new(NativeCfg);
    posthog_stateful_config_free(NativeCfg);
    if (!State->Native)
    {
        const char* Err = posthog_last_error();
        UE_LOG(LogPostHog, Error, TEXT("PostHog client creation failed: %s"), Err ? UTF8_TO_TCHAR(Err) : TEXT("unknown error"));
        return nullptr;
    }

    {
        FScopeLock Lock(&RegistryMutex);
        if (bModuleStopping) { return nullptr; }
        LiveClients.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
        LiveClients.Add(State);
    }
    return FPtr(new FPostHogClient(MoveTemp(State)));
}

TSharedFuture<FPostHogClient::FPtr> FPostHogClient::CreateAsync(FPostHogConfig Config)
{
    return RunAsync<FPtr>([Config = MoveTemp(Config)]() mutable -> FPtr
    {
        return CreateSync(MoveTemp(Config));
    });
}

FPostHogClient::~FPostHogClient()
{
    if (!Impl) { return; }
    auto Self = MoveTemp(Impl);
    const bool bNeedsShutdown = Self->Native && !Self->bClosed.exchange(true);
    LaunchTrackedTask([Self = MoveTemp(Self), bNeedsShutdown]() mutable
    {
        if (bNeedsShutdown && Self->Native)
        {
            posthog_stateful_shutdown(Self->Native, nullptr);
        }
        Self.Reset();
    });
}

bool FPostHogClient::IsInitialized() const { return Impl && Impl->Native && !Impl->bClosed.load(); }
bool FPostHogClient::IsDisabled() const { return !IsInitialized() || posthog_stateful_is_disabled(Impl->Native); }
bool FPostHogClient::IsOptedOut() const { return !IsInitialized() || posthog_stateful_is_opt_out(Impl->Native); }

EPostHogCaptureResult FPostHogClient::Capture(const FString& Event, const FPostHogProperties& Properties, const FPostHogProperties& Options)
{
    if (!IsInitialized()) { return EPostHogCaptureResult::Closed; }
    if (!Properties.IsValid() || !Options.IsValid()) { return EPostHogCaptureResult::Invalid; }
    const FTCHARToUTF8 EventUtf8(*Event);
    const FTCHARToUTF8 PropsUtf8(*Properties.ToJson());
    const FTCHARToUTF8 OptsUtf8(*Options.ToJson());
    PostHogCaptureOutcome Outcome = POSTHOG_CAPTURE_INVALID;
    posthog_stateful_capture(
        Impl->Native,
        EventUtf8.Get(),
        Properties.IsEmpty() ? nullptr : PropsUtf8.Get(),
        Options.IsEmpty() ? nullptr : OptsUtf8.Get(),
        &Outcome);
    return ToCaptureResult(Outcome);
}

EPostHogCaptureResult FPostHogClient::CaptureWithFallback(const FString& Event, const FPostHogProperties& Properties,
    const FPostHogProperties& Fallback, const FPostHogProperties& Options)
{
    if (!Properties.IsValid() || !Fallback.IsValid()) { return EPostHogCaptureResult::Invalid; }
    if (Fallback.IsEmpty()) { return Capture(Event, Properties, Options); }
    if (Properties.IsEmpty()) { return Capture(Event, Fallback, Options); }
    auto Merged = Fallback.ToNative();
    PostHogJson::Merge(*Merged, *Properties.ToNative());
    return Capture(Event, FPostHogProperties::FromNative(Merged), Options);
}

EPostHogCaptureResult FPostHogClient::Screen(const FString& Name, const FPostHogProperties& Properties)
{
    if (!IsInitialized()) { return EPostHogCaptureResult::Closed; }
    if (!Properties.IsValid()) { return EPostHogCaptureResult::Invalid; }
    PostHogCaptureOutcome Outcome = POSTHOG_CAPTURE_INVALID;
    posthog_stateful_screen(Impl->Native, TCHAR_TO_UTF8(*Name), TCHAR_TO_UTF8(*Properties.ToJson()), &Outcome);
    return ToCaptureResult(Outcome);
}

void FPostHogClient::Identify(const FString& Id, const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (!IsInitialized() || !Set.IsValid() || !SetOnce.IsValid()) { return; }
    posthog_stateful_identify(Impl->Native, TCHAR_TO_UTF8(*Id), TCHAR_TO_UTF8(*Set.ToJson()), TCHAR_TO_UTF8(*SetOnce.ToJson()));
}

void FPostHogClient::Reset() { if (IsInitialized()) { posthog_stateful_reset(Impl->Native); } }
void FPostHogClient::Alias(const FString& AliasId) { if (IsInitialized()) { posthog_stateful_alias(Impl->Native, TCHAR_TO_UTF8(*AliasId)); } }

FString FPostHogClient::GetDistinctId() const
{
    if (!IsInitialized()) { return FString(); }
    char* Raw = nullptr;
    posthog_stateful_get_distinct_id(Impl->Native, &Raw);
    return TakeOwnedString(Raw);
}

FString FPostHogClient::GetAnonymousId() const
{
    if (!IsInitialized()) { return FString(); }
    char* Raw = nullptr;
    posthog_stateful_get_anonymous_id(Impl->Native, &Raw);
    return TakeOwnedString(Raw);
}

void FPostHogClient::Register(const FPostHogProperties& Properties)
{
    if (IsInitialized() && Properties.IsValid()) { posthog_stateful_register(Impl->Native, TCHAR_TO_UTF8(*Properties.ToJson())); }
}
void FPostHogClient::Unregister(const FString& Key) { if (IsInitialized()) { posthog_stateful_unregister(Impl->Native, TCHAR_TO_UTF8(*Key)); } }

void FPostHogClient::SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (IsInitialized() && Set.IsValid() && SetOnce.IsValid())
    {
        posthog_stateful_set_person_properties(Impl->Native, TCHAR_TO_UTF8(*Set.ToJson()), TCHAR_TO_UTF8(*SetOnce.ToJson()));
    }
}

void FPostHogClient::Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties)
{
    if (IsInitialized() && Properties.IsValid())
    {
        posthog_stateful_group(Impl->Native, TCHAR_TO_UTF8(*Type), TCHAR_TO_UTF8(*Key), TCHAR_TO_UTF8(*Properties.ToJson()));
    }
}

void FPostHogClient::GroupIdentify(const FString& Type, const FString& Key, const FPostHogProperties& Properties)
{
    if (IsInitialized() && Properties.IsValid())
    {
        posthog_stateful_group_identify(Impl->Native, TCHAR_TO_UTF8(*Type), TCHAR_TO_UTF8(*Key), TCHAR_TO_UTF8(*Properties.ToJson()));
    }
}

void FPostHogClient::RemoveGroup(const FString& Type) { if (IsInitialized()) { posthog_stateful_remove_group(Impl->Native, TCHAR_TO_UTF8(*Type)); } }
void FPostHogClient::OptOut()
{
    if (!IsInitialized()) { return; }
    posthog_stateful_opt_out(Impl->Native);
    posthog_stateful_replay_stop(Impl->Native);
}
void FPostHogClient::OptIn() { if (IsInitialized()) { posthog_stateful_opt_in(Impl->Native); } }

TSharedFuture<FPostHogDrainResult> FPostHogClient::FlushAsync()
{
    if (!IsInitialized()) { return ReadyFuture(FPostHogDrainResult{}); }
    return RunAsync<FPostHogDrainResult>([Self = Impl]
    {
        PostHogDrainReport Report{POSTHOG_PERSISTENCE_NOT_REQUESTED, POSTHOG_DELIVERY_CLOSED};
        posthog_stateful_flush(Self->Native, &Report);
        return ToDrainResult(Report);
    });
}

FPostHogDrainResult FPostHogClient::ShutdownSync()
{
    if (!Impl || !Impl->Native) { return {}; }
    Impl->bClosed.store(true);
    PostHogDrainReport Report{POSTHOG_PERSISTENCE_NOT_REQUESTED, POSTHOG_DELIVERY_CLOSED};
    posthog_stateful_shutdown(Impl->Native, &Report);
    return ToDrainResult(Report);
}

TSharedFuture<FPostHogDrainResult> FPostHogClient::ShutdownAsync()
{
    if (!Impl || !Impl->Native) { return ReadyFuture(FPostHogDrainResult{}); }
    Impl->bClosed.store(true);
    return RunAsync<FPostHogDrainResult>([Self = Impl]
    {
        PostHogDrainReport Report{POSTHOG_PERSISTENCE_NOT_REQUESTED, POSTHOG_DELIVERY_CLOSED};
        posthog_stateful_shutdown(Self->Native, &Report);
        return ToDrainResult(Report);
    });
}

FPostHogFeatureFlagResult FPostHogClient::GetFeatureFlagResult(const FString& Key, EPostHogFlagReporting Reporting)
{
    if (!IsInitialized()) { return {}; }
    char* Raw = nullptr;
    if (posthog_stateful_get_feature_flag_json(Impl->Native, TCHAR_TO_UTF8(*Key), ToNativeReporting(Reporting), &Raw) != POSTHOG_STATUS_OK || !Raw)
    {
        return {};
    }
    return ParseFlagResult(PostHogJson::DecodeValue(TakeOwnedString(Raw), MaxFlagJsonBytes));
}

bool FPostHogClient::IsFeatureEnabled(const FString& Key, EPostHogFlagReporting Reporting, bool bDefaultValue)
{
    const auto Flag = GetFeatureFlagResult(Key, Reporting);
    return Flag.bFound ? Flag.bEnabled : bDefaultValue;
}

FPostHogFeatureFlags FPostHogClient::GetFeatureFlags() const
{
    if (!IsInitialized()) { return {}; }
    char* Raw = nullptr;
    if (posthog_stateful_get_feature_flags_json(Impl->Native, &Raw) != POSTHOG_STATUS_OK || !Raw) { return {}; }
    return ParseFlagMap(PostHogJson::DecodeValue(TakeOwnedString(Raw), MaxFlagJsonBytes));
}

void FPostHogClient::ReloadFeatureFlags() { if (IsInitialized()) { posthog_stateful_reload_feature_flags(Impl->Native); } }

void FPostHogClient::SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload)
{
    if (IsInitialized() && Properties.IsValid())
    {
        posthog_stateful_set_person_properties_for_flags(Impl->Native, TCHAR_TO_UTF8(*Properties.ToJson()), bReload);
    }
}

void FPostHogClient::SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload)
{
    if (IsInitialized() && Properties.IsValid())
    {
        posthog_stateful_set_group_properties_for_flags(Impl->Native, TCHAR_TO_UTF8(*Type), TCHAR_TO_UTF8(*Properties.ToJson()), bReload);
    }
}

void FPostHogClient::ResetPersonPropertiesForFlags(bool bReload)
{
    if (IsInitialized()) { posthog_stateful_reset_person_properties_for_flags(Impl->Native, bReload); }
}

void FPostHogClient::ResetGroupPropertiesForFlags(const FString& Type, bool bReload)
{
    if (IsInitialized())
    {
        posthog_stateful_reset_group_properties_for_flags(Impl->Native, Type.IsEmpty() ? nullptr : TCHAR_TO_UTF8(*Type), bReload);
    }
}

FString FPostHogClient::GetSessionId()
{
    if (!IsInitialized()) { return FString(); }
    char* Raw = nullptr;
    posthog_stateful_get_session_id(Impl->Native, &Raw);
    return TakeOwnedString(Raw);
}

FString FPostHogClient::PeekSessionId() const
{
    if (!IsInitialized()) { return FString(); }
    char* Raw = nullptr;
    posthog_stateful_peek_session_id(Impl->Native, &Raw);
    return TakeOwnedString(Raw);
}

void FPostHogClient::StartSession() { if (IsInitialized()) { posthog_stateful_start_session(Impl->Native); } }
void FPostHogClient::EndSession() { if (IsInitialized()) { posthog_stateful_end_session(Impl->Native); } }
void FPostHogClient::ResetSession() { if (IsInitialized()) { posthog_stateful_reset_session(Impl->Native); } }
void FPostHogClient::SessionActivity() { if (IsInitialized()) { posthog_stateful_session_activity(Impl->Native); } }

void FPostHogClient::SetAppInBackground(bool bBackground)
{
    if (!IsInitialized()) { return; }
    Impl->bForeground.store(!bBackground);
    posthog_stateful_set_app_in_background(Impl->Native, bBackground);
}

void FPostHogClient::StartSessionReplay()
{
    if (IsInitialized() && !IsDisabled() && !IsOptedOut()) { posthog_stateful_replay_start(Impl->Native); }
}
void FPostHogClient::StopSessionReplay() { if (IsInitialized()) { posthog_stateful_replay_stop(Impl->Native); } }
bool FPostHogClient::IsSessionReplayActive() const { return IsInitialized() && posthog_stateful_replay_is_active(Impl->Native); }

FPostHogClient::EReplayFrameStatus FPostHogClient::BeginReplayFrame(int32 Width, int32 Height, int32 RowStrideBytes, TUniquePtr<FPostHogReplayFrame>& OutFrame)
{
    OutFrame.Reset();
    if (!IsInitialized() || !Impl->bForeground.load() || Width <= 0 || Height <= 0
        || static_cast<int64>(RowStrideBytes) < static_cast<int64>(Width) * 4)
    {
        return EReplayFrameStatus::Inactive;
    }
    PostHogReplayFrame* NativeFrame = nullptr;
    PostHogFrameUnavailable Reason = POSTHOG_FRAME_UNKNOWN;
    if (posthog_stateful_replay_begin_frame(Impl->Native, static_cast<uint32>(Width), static_cast<uint32>(Height),
        static_cast<size_t>(RowStrideBytes), &NativeFrame, &Reason) != POSTHOG_STATUS_OK)
    {
        return EReplayFrameStatus::Inactive;
    }
    if (NativeFrame)
    {
        OutFrame.Reset(new FPostHogReplayFrame(NativeFrame, Impl));
        return EReplayFrameStatus::Ok;
    }
    return Reason == POSTHOG_FRAME_THROTTLED ? EReplayFrameStatus::Throttled : EReplayFrameStatus::Inactive;
}

FPostHogReplayFrame::~FPostHogReplayFrame() { Abandon(); }

void FPostHogReplayFrame::Abandon()
{
    if (Frame)
    {
        posthog_stateful_replay_frame_free(Frame);
        Frame = nullptr;
    }
}

bool FPostHogReplayFrame::Submit(const uint8* Pixels, int64 NumBytes)
{
    PostHogReplayFrame* NativeFrame = Frame;
    Frame = nullptr;
    if (!NativeFrame) { return false; }
    auto State = Owner.Pin();
    if (!State || !State->Native || !Pixels || NumBytes < 0)
    {
        posthog_stateful_replay_frame_free(NativeFrame);
        return false;
    }
    PostHogSubmitOutcome Outcome = POSTHOG_SUBMIT_CLOSED;
    const auto Status = posthog_stateful_replay_submit_frame(
        State->Native, NativeFrame, Pixels, static_cast<size_t>(NumBytes), &Outcome);
    return Status == POSTHOG_STATUS_OK && Outcome == POSTHOG_SUBMIT_ACCEPTED;
}

void FPostHogClient::ShutdownAll()
{
    TArray<TSharedPtr<FImpl, ESPMode::ThreadSafe>> Active;
    FEvent* IdleEvent = nullptr;
    {
        FScopeLock Lock(&RegistryMutex);
        bModuleStopping = true;
        for (const auto& Weak : LiveClients)
        {
            if (auto Pin = Weak.Pin()) { Active.Add(MoveTemp(Pin)); }
        }
        LiveClients.Empty();
        IdleEvent = TasksIdleEvent;
    }
    for (const auto& State : Active)
    {
        State->bClosed.store(true);
        if (State->Native) { posthog_stateful_shutdown(State->Native, nullptr); }
    }
    Active.Empty();
    if (IdleEvent)
    {
        IdleEvent->Wait();
        FScopeLock Lock(&RegistryMutex);
        if (TasksIdleEvent)
        {
            FPlatformProcess::ReturnSynchEventToPool(TasksIdleEvent);
            TasksIdleEvent = nullptr;
        }
    }
}
