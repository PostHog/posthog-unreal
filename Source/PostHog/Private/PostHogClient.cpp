#include "PostHogClient.h"
#include "PostHogNative.h"
#include "PostHogJson.h"
#include "PostHogTasks.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogClient, Log, All);
FCriticalSection FPostHogClient::ClientsMutex;
TArray<TSharedPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe>> FPostHogClient::Clients;
bool FPostHogClient::bStopping = false;

namespace
{
template <typename T> TSharedFuture<T> Ready(T Value)
{ TPromise<T> Promise; auto Future = Promise.GetFuture().Share(); Promise.SetValue(MoveTemp(Value)); return Future; }
FPostHogDrainResult Failure(EPostHogResult Status)
{ FPostHogDrainResult Out; Out.Status = Status; return Out; }
}

struct FPostHogClient::FImpl : TSharedFromThis<FImpl, ESPMode::ThreadSafe>
{
    FCriticalSection Mutex;
    phu_handle Handle = 0;
    bool bClosing = false;
    bool bClosed = false;
    bool bCloseScheduled = false;
    bool bForeground = true;
    int32 ActiveCalls = 0;
    FEvent* Idle = FPlatformProcess::GetSynchEventFromPool(true);
    TFunction<bool(FJsonObject&)> BeforeSend;
    TFunction<void()> CancelHost;
    TFunction<FHostClose()> RetireHost;
    TSharedFuture<FPostHogDrainResult> CloseFuture;
    FPostHogDrainResult LastClose;
    static thread_local TArray<FImpl*> HookClients;

    explicit FImpl(TFunction<bool(FJsonObject&)>&& Hook) : BeforeSend(MoveTemp(Hook)) { Idle->Trigger(); }
    ~FImpl() { FPlatformProcess::ReturnSynchEventToPool(Idle); }

    /** Hold the callback context through host conversion AND the native call. Never hold Mutex in the SDK. */
    struct FCall
    {
        TSharedPtr<FImpl, ESPMode::ThreadSafe> Owner;
        phu_handle Handle = 0;
        explicit FCall(TSharedPtr<FImpl, ESPMode::ThreadSafe> InOwner) : Owner(MoveTemp(InOwner))
        {
            FScopeLock Lock(&Owner->Mutex);
            if (!Owner->bClosing && Owner->Handle)
            { if (Owner->ActiveCalls++ == 0) { Owner->Idle->Reset(); } Handle = Owner->Handle; }
        }
        ~FCall()
        {
            if (!Handle) { return; }
            FScopeLock Lock(&Owner->Mutex);
            if (--Owner->ActiveCalls == 0) { Owner->Idle->Trigger(); }
        }
    };

    static phu_status Hook(void* UserData, phu_bytes Input, phu_buffer* Output)
    {
        *Output = {nullptr, 0};
        auto& Self = *static_cast<FImpl*>(UserData);
        struct FHookScope
        {
            explicit FHookScope(FImpl* Current) { HookClients.Add(Current); }
            ~FHookScope() { HookClients.Pop(EAllowShrinking::No); }
        } Scope(&Self);
        if (!Input.data || Input.len > PHU_MAX_HOOK) { return PHU_DROPPED; }
        const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Input.data), static_cast<int32>(Input.len));
        auto Event = PostHogJson::Decode(FString(Text.Length(), Text.Get()), PHU_MAX_HOOK);
        if (!Event || !Self.BeforeSend) { return PHU_DROPPED; }
#if PLATFORM_EXCEPTIONS_DISABLED
        if (!Self.BeforeSend(*Event)) { return PHU_DROPPED; }
#else
        try { if (!Self.BeforeSend(*Event)) { return PHU_DROPPED; } }
        catch (...) { return PHU_DROPPED; }
#endif
        const FString Json = PostHogJson::Encode(Event.ToSharedRef());
        if (!PostHogJson::IsBounded(Json, PHU_MAX_HOOK)) { return PHU_DROPPED; }
        const FTCHARToUTF8 Utf8(*Json);
        return phu_buffer_copy(PostHogNative::View(Utf8), Output);
    }
    bool Open()
    { FScopeLock Lock(&Mutex); return Handle && !bClosing; }
    EPostHogResult Command(const TSharedRef<FJsonObject>& Object)
    {
        const FString Json = PostHogJson::Encode(Object);
        if (!PostHogJson::IsBounded(Json)) { return EPostHogResult::InvalidInput; }
        FCall Call(AsShared()); if (!Call.Handle) { return EPostHogResult::Closed; }
        const FTCHARToUTF8 Utf8(*Json);
        return PostHogNative::Result(phu_command(Call.Handle, PostHogNative::View(Utf8)));
    }
    EPostHogResult Read(const TSharedRef<FJsonObject>& Query, TSharedPtr<FJsonValue>& Value)
    {
        FCall Call(AsShared()); if (!Call.Handle) { return EPostHogResult::Closed; }
        const FString Json = PostHogJson::Encode(Query);
        if (!PostHogJson::IsBounded(Json)) { return EPostHogResult::InvalidInput; }
        const FTCHARToUTF8 Utf8(*Json); PostHogNative::FBuffer Buffer;
        const auto Status = phu_read(Call.Handle, PostHogNative::View(Utf8), &Buffer.Value);
        if (Status != PHU_OK) { return PostHogNative::Result(Status); }
        Value = Buffer.Read(); return Value ? EPostHogResult::Ok : EPostHogResult::InvalidResponse;
    }
    TSharedPtr<FJsonObject> State()
    {
        TSharedPtr<FJsonValue> Value;
        return Read(PostHogNative::Operation(TEXT("state")), Value) == EPostHogResult::Ok && Value->Type == EJson::Object
            ? Value->AsObject() : nullptr;
    }
    void BeginClose()
    {
        phu_handle NativeHandle; TFunction<void()> Cancel;
        { FScopeLock Lock(&Mutex); bClosing = true; NativeHandle = Handle; Cancel = CancelHost; }
        // Cancel synchronizes renderer SDK calls before native admission closes. No GPU wait here.
        if (Cancel) { Cancel(); }
        if (NativeHandle) { phu_begin_shutdown(NativeHandle); }
    }
    FPostHogDrainResult FinishClose()
    {
        phu_handle NativeHandle;
        { FScopeLock Lock(&Mutex); if (bClosed) { return LastClose; } NativeHandle = Handle; }
        TFunction<FHostClose()> Retire;
        { FScopeLock Lock(&Mutex); Retire = RetireHost; }
        if (Retire)
        {
            const auto Host = Retire();
            if (!Host.Key.IsValid() || !Host.Key.Get()) { return Failure(EPostHogResult::TaskUnavailable); }
        }
        PostHogNative::FBuffer Buffer;
        const auto Status = phu_close(NativeHandle, &Buffer.Value);
        auto Result = PostHogNative::Drain(Status, Buffer);
        if (Status != PHU_OK) { return Result; } // CLOSED/DEFERRED/error do not certify teardown.
        Idle->Wait();
        TFunction<bool(FJsonObject&)> ReleasedHook;
        TFunction<void()> ReleasedCancel; TFunction<FHostClose()> ReleasedRetire;
        {
            FScopeLock Lock(&Mutex);
            Handle = 0; bClosed = true; Result.bJoined = true; LastClose = Result;
            ReleasedHook = MoveTemp(BeforeSend);
            ReleasedCancel = MoveTemp(CancelHost); ReleasedRetire = MoveTemp(RetireHost);
        }
        {
            FScopeLock Lock(&ClientsMutex);
            Clients.RemoveAll([this](const auto& Entry) { return Entry.Get() == this; });
        }
        // Application captures can have reentrant destructors; release outside all locks.
        ReleasedHook = nullptr;
        return Result;
    }
    TSharedFuture<FPostHogDrainResult> Close()
    {
        BeginClose();
        TFunction<FHostClose()> Retire;
        { FScopeLock Lock(&Mutex); Retire = RetireHost; }
        const bool bHostDeferred = Retire && Retire().Value;
        TSharedFuture<FPostHogDrainResult> Future;
        {
            FScopeLock Lock(&Mutex);
            if (bClosed) { return Ready(LastClose); }
            if (!bCloseScheduled)
            {
                auto Promise = MakeShared<TPromise<FPostHogDrainResult>, ESPMode::ThreadSafe>();
                CloseFuture = Promise->GetFuture().Share(); bCloseScheduled = true;
                auto Self = AsShared();
                if (!PostHogTasks::Launch([Self, Promise]
                {
                    auto Result = Self->FinishClose();
                    { FScopeLock StateLock(&Self->Mutex); Self->bCloseScheduled = false; }
                    Promise->SetValue(Result);
                }))
                { bCloseScheduled = false; Promise->SetValue(Failure(EPostHogResult::TaskUnavailable)); }
            }
            Future = CloseFuture;
        }
        // Waiting on a background finisher inside the callback would wait for this very callback.
        return HookClients.Contains(this) || bHostDeferred ? Ready(Failure(EPostHogResult::Deferred)) : Future;
    }
};
thread_local TArray<FPostHogClient::FImpl*> FPostHogClient::FImpl::HookClients;

bool FPostHogClient::BindHostClose(TFunction<void()>&& Cancel, TFunction<FHostClose()>&& Retire)
{
    FScopeLock Lock(&Impl->Mutex);
    if (Impl->bClosing || Impl->RetireHost) { return false; }
    Impl->CancelHost = MoveTemp(Cancel); Impl->RetireHost = MoveTemp(Retire); return true;
}

EPostHogResult FPostHogClient::CoordinationKey(const FPostHogConfig& Config, FString& OutKey)
{
    OutKey.Empty();
    if (phu_abi_version() != PHU_ABI_VERSION) { return EPostHogResult::UnsupportedAbi; }
    auto Input = MakeShared<FJsonObject>();
    Input->SetStringField(TEXT("host"), Config.Host); Input->SetStringField(TEXT("project_token"), Config.ProjectToken);
    Input->SetStringField(TEXT("instance_name"), Config.ClientName);
    const FString Json = PostHogJson::Encode(Input);
    if (!PostHogJson::IsBounded(Json)) { return EPostHogResult::InvalidInput; }
    const FTCHARToUTF8 Utf8(*Json); PostHogNative::FBuffer Buffer;
    const auto Status = phu_coordination_key(PostHogNative::View(Utf8), &Buffer.Value);
    if (Status != PHU_OK) { return PostHogNative::Result(Status); }
    const auto Value = Buffer.Read();
    if (!Value || Value->Type != EJson::String) { return EPostHogResult::InvalidResponse; }
    OutKey = Value->AsString(); return EPostHogResult::Ok;
}

TSharedFuture<FPostHogClient::FPtr> FPostHogClient::CreateAsync(FPostHogConfig Config)
{
    FScopeLock Admission(&ClientsMutex);
    if (bStopping) { return Ready(FPtr()); }
    return PostHogTasks::Run([Config = MoveTemp(Config)]() mutable -> FPtr
    {
        if (phu_abi_version() != PHU_ABI_VERSION || !Config.Context.IsValid()) { return nullptr; }
        auto State = MakeShared<FImpl, ESPMode::ThreadSafe>(MoveTemp(Config.BeforeSend));
        auto Options = MakeShared<FJsonObject>();
        Options->SetStringField(TEXT("project_token"), Config.ProjectToken);
        Options->SetStringField(TEXT("host"), Config.Host);
        if (!Config.StorageRoot.IsEmpty()) { Options->SetStringField(TEXT("storage_root"), Config.StorageRoot); }
        Options->SetStringField(TEXT("instance_name"), Config.ClientName);
        const TCHAR* Profiles = Config.PersonProfiles == EPostHogPersonProfiles::Never ? TEXT("never") :
            Config.PersonProfiles == EPostHogPersonProfiles::Always ? TEXT("always") : TEXT("identified_only");
        Options->SetStringField(TEXT("person_profiles"), Profiles);
        Options->SetStringField(TEXT("session_mode"), Config.SessionMode == EPostHogSessionMode::Manual ? TEXT("manual") : TEXT("automatic"));
        Options->SetBoolField(TEXT("persist_sessions"), Config.bPersistSessions);
        Options->SetBoolField(TEXT("opt_out"), Config.bInitiallyOptedOut);
        Options->SetBoolField(TEXT("preload_feature_flags"), Config.bPreloadFeatureFlags);
        Options->SetBoolField(TEXT("send_feature_flag_event"), Config.bSendFeatureFlagEvents);
        Options->SetObjectField(TEXT("platform_context"), Config.Context.ToNative());
        Options->SetNumberField(TEXT("flush_at"), Config.FlushAt);
        Options->SetNumberField(TEXT("flush_interval_ms"), Config.FlushIntervalMilliseconds);
        Options->SetNumberField(TEXT("max_queue_size"), Config.MaxQueueSize);
        Options->SetNumberField(TEXT("max_batch_size"), Config.MaxBatchSize);
        Options->SetNumberField(TEXT("request_timeout_seconds"), Config.RequestTimeoutSeconds);
        Options->SetNumberField(TEXT("shutdown_timeout_ms"), Config.ShutdownTimeoutMilliseconds);
        Options->SetNumberField(TEXT("jpeg_quality"), Config.Replay.JpegQuality);
        Options->SetNumberField(TEXT("max_frames_in_flight"), Config.Replay.MaxFramesInFlight);
        const FString Json = PostHogJson::Encode(Options);
        if (!PostHogJson::IsBounded(Json)) { return nullptr; }
        const FTCHARToUTF8 Utf8(*Json);
        const phu_hooks Hooks{State->BeforeSend ? &FImpl::Hook : nullptr, &State.Get()};
        if (phu_create(PHU_ABI_VERSION, PostHogNative::View(Utf8), Hooks, &State->Handle) != PHU_OK) { return nullptr; }
        bool bStop;
        { FScopeLock Lock(&ClientsMutex); bStop = bStopping; Clients.Add(State); }
        if (bStop)
        {
            State->BeginClose();
            if (!State->FinishClose().bJoined) { UE_LOG(LogPostHogClient, Fatal, TEXT("PostHog initialization could not join native teardown.")); }
            return nullptr;
        }
        return FPtr(new FPostHogClient(State));
    });
}
FPostHogClient::~FPostHogClient() { Impl->Close(); }
bool FPostHogClient::IsInitialized() const { return Impl->Open(); }
bool FPostHogClient::IsDisabled() const
{ const auto State = Impl->State(); bool Disabled = true; if (State) { State->TryGetBoolField(TEXT("disabled"), Disabled); } return Disabled; }
bool FPostHogClient::IsOptedOut() const
{ const auto State = Impl->State(); bool OptedOut = true; if (State) { State->TryGetBoolField(TEXT("opt_out"), OptedOut); } return OptedOut; }
FString FPostHogClient::GetDistinctId() const
{ const auto State = Impl->State(); FString Id; if (State) { State->TryGetStringField(TEXT("distinct_id"), Id); } return Id; }
FString FPostHogClient::GetAnonymousId() const
{ const auto State = Impl->State(); FString Id; if (State) { State->TryGetStringField(TEXT("anonymous_id"), Id); } return Id; }

EPostHogResult FPostHogClient::Capture(const FString& Name, const FPostHogProperties& Properties, const FPostHogProperties& Options)
{ return CaptureWithFallback(Name, Properties, {}, Options); }
EPostHogResult FPostHogClient::CaptureWithFallback(const FString& Name, const FPostHogProperties& Properties,
    const FPostHogProperties& Fallback, const FPostHogProperties& Options)
{
    if (!Properties.IsValid() || !Options.IsValid() || !Fallback.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("event"), Name);
    Input->SetObjectField(TEXT("properties"), Properties.ToNative()); Input->SetObjectField(TEXT("options"), Options.ToNative());
    Input->SetObjectField(TEXT("fallback_properties"), Fallback.ToNative());
    const FString Json = PostHogJson::Encode(Input);
    if (!PostHogJson::IsBounded(Json)) { return EPostHogResult::InvalidInput; }
    FImpl::FCall Call(Impl); if (!Call.Handle) { return EPostHogResult::Closed; }
    const FTCHARToUTF8 Utf8(*Json); PostHogNative::FBuffer Receipt;
    return PostHogNative::Result(phu_capture(Call.Handle, PostHogNative::View(Utf8), &Receipt.Value));
}
EPostHogResult FPostHogClient::Screen(const FString& Name, const FPostHogProperties& Properties)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = MakeShared<FJsonObject>(); Input->SetStringField(TEXT("name"), Name); Input->SetObjectField(TEXT("properties"), Properties.ToNative());
    const FString Json = PostHogJson::Encode(Input); if (!PostHogJson::IsBounded(Json)) { return EPostHogResult::InvalidInput; }
    FImpl::FCall Call(Impl); if (!Call.Handle) { return EPostHogResult::Closed; }
    const FTCHARToUTF8 Utf8(*Json); PostHogNative::FBuffer Receipt;
    return PostHogNative::Result(phu_screen(Call.Handle, PostHogNative::View(Utf8), &Receipt.Value));
}
EPostHogResult FPostHogClient::Identify(const FString& Id, const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (!Set.IsValid() || !SetOnce.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("identify")); Input->SetStringField(TEXT("distinct_id"), Id);
    Input->SetObjectField(TEXT("set"), Set.ToNative()); Input->SetObjectField(TEXT("set_once"), SetOnce.ToNative()); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::Reset() { return Impl->Command(PostHogNative::Operation(TEXT("reset"))); }
EPostHogResult FPostHogClient::Alias(const FString& Id)
{ auto Input = PostHogNative::Operation(TEXT("alias")); Input->SetStringField(TEXT("alias"), Id); return Impl->Command(Input); }
EPostHogResult FPostHogClient::Register(const FPostHogProperties& Properties)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("register")); Input->SetObjectField(TEXT("properties"), Properties.ToNative()); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::Unregister(const FString& Key)
{ auto Input = PostHogNative::Operation(TEXT("unregister")); Input->SetStringField(TEXT("key"), Key); return Impl->Command(Input); }
EPostHogResult FPostHogClient::SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (!Set.IsValid() || !SetOnce.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("set_person_properties"));
    Input->SetObjectField(TEXT("set"), Set.ToNative()); Input->SetObjectField(TEXT("set_once"), SetOnce.ToNative()); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("group")); Input->SetStringField(TEXT("group_type"), Type);
    Input->SetStringField(TEXT("group_key"), Key); Input->SetObjectField(TEXT("properties"), Properties.ToNative()); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::GroupIdentify(const FString& Type, const FString& Key, const FPostHogProperties& Properties)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("group_identify")); Input->SetStringField(TEXT("group_type"), Type);
    Input->SetStringField(TEXT("group_key"), Key); Input->SetObjectField(TEXT("properties"), Properties.ToNative()); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::RemoveGroup(const FString& Type)
{ auto Input = PostHogNative::Operation(TEXT("remove_group")); Input->SetStringField(TEXT("group_type"), Type); return Impl->Command(Input); }
EPostHogResult FPostHogClient::OptOut() { return Impl->Command(PostHogNative::Operation(TEXT("opt_out"))); }
EPostHogResult FPostHogClient::OptIn() { return Impl->Command(PostHogNative::Operation(TEXT("opt_in"))); }
TSharedFuture<FPostHogDrainResult> FPostHogClient::FlushAsync()
{
    auto Flush = [Self = Impl]
    {
        FImpl::FCall Call(Self); if (!Call.Handle) { return Failure(EPostHogResult::Closed); }
        PostHogNative::FBuffer Buffer; return PostHogNative::Drain(phu_flush(Call.Handle, &Buffer.Value), Buffer);
    };
    if (FImpl::HookClients.Contains(Impl.Get())) { return Ready(Flush()); }
    auto Promise = MakeShared<TPromise<FPostHogDrainResult>, ESPMode::ThreadSafe>(); auto Future = Promise->GetFuture().Share();
    if (!PostHogTasks::Launch([Promise, Flush = MoveTemp(Flush)] { Promise->SetValue(Flush()); }))
    { Promise->SetValue(Failure(EPostHogResult::TaskUnavailable)); }
    return Future;
}
TSharedFuture<FPostHogDrainResult> FPostHogClient::ShutdownAsync() { return Impl->Close(); }

FPostHogFeatureFlagResult FPostHogClient::GetFeatureFlagResult(const FString& Key, EPostHogFlagReporting Reporting)
{
    auto Query = PostHogNative::Operation(TEXT("feature_flag")); Query->SetStringField(TEXT("key"), Key);
    if (Reporting != EPostHogFlagReporting::Default) { Query->SetBoolField(TEXT("send_event"), Reporting == EPostHogFlagReporting::Send); }
    TSharedPtr<FJsonValue> Value; const auto Status = Impl->Read(Query, Value);
    if (Status == EPostHogResult::Ok) { return PostHogNative::Flag(Value); }
    FPostHogFeatureFlagResult Out; Out.Status = Status; return Out;
}
FPostHogFeatureFlags FPostHogClient::GetFeatureFlags() const
{
    TSharedPtr<FJsonValue> Value; const auto Status = Impl->Read(PostHogNative::Operation(TEXT("feature_flags")), Value);
    if (Status == EPostHogResult::Ok) { return PostHogNative::Flags(Value); }
    FPostHogFeatureFlags Out; Out.Status = Status; return Out;
}
EPostHogResult FPostHogClient::ReloadFeatureFlags() { return Impl->Command(PostHogNative::Operation(TEXT("reload_feature_flags"))); }
EPostHogResult FPostHogClient::PollFeatureFlags(FPostHogFeatureFlagsUpdate& Update)
{
    Update = {}; FImpl::FCall Call(Impl);
    if (!Call.Handle) { Update.Snapshot.Status = EPostHogResult::Closed; return EPostHogResult::Closed; }
    PostHogNative::FBuffer Buffer; const auto Status = phu_poll_flags(Call.Handle, &Buffer.Value);
    if (Status != PHU_OK) { Update.Snapshot.Status = PostHogNative::Result(Status); return Update.Snapshot.Status; }
    Update = PostHogNative::Update(Buffer.Read()); return Update.Snapshot.Status;
}
EPostHogResult FPostHogClient::SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("set_person_properties_for_flags")); Input->SetObjectField(TEXT("properties"), Properties.ToNative());
    Input->SetBoolField(TEXT("reload"), bReload); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload)
{
    if (!Properties.IsValid()) { return EPostHogResult::InvalidInput; }
    auto Input = PostHogNative::Operation(TEXT("set_group_properties_for_flags")); Input->SetStringField(TEXT("group_type"), Type);
    Input->SetObjectField(TEXT("properties"), Properties.ToNative()); Input->SetBoolField(TEXT("reload"), bReload); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::ResetPersonPropertiesForFlags(bool bReload)
{ auto Input = PostHogNative::Operation(TEXT("reset_person_properties_for_flags")); Input->SetBoolField(TEXT("reload"), bReload); return Impl->Command(Input); }
EPostHogResult FPostHogClient::ResetGroupPropertiesForFlags(const FString& Type, bool bReload)
{
    auto Input = PostHogNative::Operation(TEXT("reset_group_properties_for_flags")); if (!Type.IsEmpty()) { Input->SetStringField(TEXT("group_type"), Type); }
    Input->SetBoolField(TEXT("reload"), bReload); return Impl->Command(Input);
}
FString FPostHogClient::GetSessionId()
{ TSharedPtr<FJsonValue> Value; return Impl->Read(PostHogNative::Operation(TEXT("session_id")), Value) == EPostHogResult::Ok && Value->Type == EJson::String ? Value->AsString() : FString(); }
FString FPostHogClient::PeekSessionId() const
{ TSharedPtr<FJsonValue> Value; return Impl->Read(PostHogNative::Operation(TEXT("peek_session_id")), Value) == EPostHogResult::Ok && Value->Type == EJson::String ? Value->AsString() : FString(); }
EPostHogResult FPostHogClient::StartSession() { return Impl->Command(PostHogNative::Operation(TEXT("start_session"))); }
EPostHogResult FPostHogClient::EndSession() { return Impl->Command(PostHogNative::Operation(TEXT("end_session"))); }
EPostHogResult FPostHogClient::ResetSession() { return Impl->Command(PostHogNative::Operation(TEXT("reset_session"))); }
EPostHogResult FPostHogClient::SessionActivity() { return Impl->Command(PostHogNative::Operation(TEXT("session_activity"))); }
EPostHogResult FPostHogClient::SetAppInBackground(bool bBackground)
{
    { FScopeLock Lock(&Impl->Mutex); Impl->bForeground = !bBackground; }
    auto Input = PostHogNative::Operation(TEXT("set_app_in_background")); Input->SetBoolField(TEXT("background"), bBackground); return Impl->Command(Input);
}
EPostHogResult FPostHogClient::StartSessionReplay() { return Impl->Command(PostHogNative::Operation(TEXT("replay_start"))); }
EPostHogResult FPostHogClient::StopSessionReplay() { return Impl->Command(PostHogNative::Operation(TEXT("replay_stop"))); }
bool FPostHogClient::IsSessionReplayActive() const
{ TSharedPtr<FJsonValue> Value; return Impl->Read(PostHogNative::Operation(TEXT("replay_active")), Value) == EPostHogResult::Ok && Value->Type == EJson::Boolean && Value->AsBool(); }
EPostHogResult FPostHogClient::BeginReplayFrame(int32 Width, int32 Height, int32 RowStrideBytes, TUniquePtr<FPostHogReplayFrame>& OutFrame)
{
    OutFrame.Reset();
    if (Width <= 0 || Height <= 0 || RowStrideBytes < static_cast<int64>(Width) * 4) { return EPostHogResult::InvalidInput; }
    { FScopeLock Lock(&Impl->Mutex); if (!Impl->bForeground) { return EPostHogResult::Inactive; } }
    FImpl::FCall Call(Impl); if (!Call.Handle) { return EPostHogResult::Closed; }
    auto Layout = MakeShared<FJsonObject>(); Layout->SetNumberField(TEXT("width"), Width); Layout->SetNumberField(TEXT("height"), Height);
    Layout->SetNumberField(TEXT("row_stride_bytes"), RowStrideBytes); const FTCHARToUTF8 Utf8(*PostHogJson::Encode(Layout)); phu_frame Frame = 0;
    const auto Status = phu_begin_frame(Call.Handle, PostHogNative::View(Utf8), &Frame);
    if (Status == PHU_OK) { OutFrame.Reset(new FPostHogReplayFrame(Frame, static_cast<int64>(RowStrideBytes) * Height, Impl)); }
    return PostHogNative::Result(Status);
}
FPostHogReplayFrame::~FPostHogReplayFrame() { Abandon(); }
EPostHogResult FPostHogReplayFrame::Abandon()
{
    const auto Frame = Token; Token = 0;
    auto State = Owner.Pin(); if (!Frame || !State) { return EPostHogResult::Closed; }
    FPostHogClient::FImpl::FCall Call(State); if (!Call.Handle) { return EPostHogResult::Closed; }
    return PostHogNative::Result(phu_abandon_frame(Frame));
}
EPostHogResult FPostHogReplayFrame::Submit(const uint8* Pixels, int64 NumBytes)
{
    const auto Frame = Token; Token = 0;
    auto State = Owner.Pin(); if (!Frame || !State) { return EPostHogResult::Closed; }
    FPostHogClient::FImpl::FCall Call(State); if (!Call.Handle) { return EPostHogResult::Closed; }
    if (NumBytes < 0) { phu_abandon_frame(Frame); return EPostHogResult::InvalidInput; }
    return PostHogNative::Result(phu_submit_frame(Call.Handle, Frame, {Pixels, static_cast<size_t>(NumBytes)}));
}
void FPostHogClient::ShutdownAll()
{
    TArray<TSharedPtr<FImpl, ESPMode::ThreadSafe>> Live;
    { FScopeLock Lock(&ClientsMutex); bStopping = true; Live = Clients; }
    for (const auto& State : Live) { State->BeginClose(); }
    for (const auto& State : Live) { State->Close(); }
    PostHogTasks::Drain();
    { FScopeLock Lock(&ClientsMutex); Live = Clients; }
    // If host thread creation failed, the strong registry still owns its callback context.
    for (const auto& State : Live)
    {
        State->BeginClose();
        if (!State->FinishClose().bJoined) { UE_LOG(LogPostHogClient, Fatal, TEXT("PostHog cannot unload with unfinished native work.")); }
    }
}
