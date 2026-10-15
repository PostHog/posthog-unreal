#include "PostHogClient.h"
#include "PostHogJson.h"
#include "PostHogFlagCache.h"
#include "PostHogReplay.h"
#include "PostHogTasks.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/App.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/SecureHash.h"
#include "posthog_unreal.h"

DEFINE_LOG_CATEGORY_STATIC(LogPostHogClient, Log, All);

FCriticalSection FPostHogClient::ClientsMutex;
TArray<TWeakPtr<FPostHogClient::FImpl, ESPMode::ThreadSafe>> FPostHogClient::Clients;
bool FPostHogClient::bStopping = false;

namespace
{
template <typename T> TSharedFuture<T> Ready(T Value)
{
    TPromise<T> Promise; auto Future = Promise.GetFuture().Share(); Promise.SetValue(MoveTemp(Value)); return Future;
}
bool NonBlank(const FString& Text) { return !Text.TrimStartAndEnd().IsEmpty(); }
phu_bytes View(const FTCHARToUTF8& Text) { return {reinterpret_cast<const uint8_t*>(Text.Get()), static_cast<size_t>(Text.Length())}; }
EPostHogCaptureResult FromNative(phu_status Status)
{
    switch (Status)
    {
    case PHU_ACCEPTED: return EPostHogCaptureResult::Accepted;
    case PHU_DISABLED: return EPostHogCaptureResult::Disabled;
    case PHU_QUEUE_FULL: return EPostHogCaptureResult::QueueFull;
    case PHU_CLOSED: return EPostHogCaptureResult::Closed;
    default: return EPostHogCaptureResult::Invalid;
    }
}

bool SaveAtomic(const FString& Filename, const FString& Json)
{
    auto& Files = FPlatformFileManager::Get().GetPlatformFile();
    if (!Files.CreateDirectoryTree(*FPaths::GetPath(Filename))) { return false; }
    const FString Temporary = Filename + TEXT(".tmp");
    const FTCHARToUTF8 Text(*Json);
    TUniquePtr<IFileHandle> File(Files.OpenWrite(*Temporary));
    if (!File || !File->Write(reinterpret_cast<const uint8*>(Text.Get()), Text.Length()) || !File->Flush(true)) { return false; }
    File.Reset();
    return Files.MoveFile(*Filename, *Temporary);
}

bool SameProperties(const FPostHogProperties& Left, const FPostHogProperties& Right)
{
    return FJsonValue::CompareEqual(FJsonValueObject(Left.ToNative()), FJsonValueObject(Right.ToNative()));
}
}

struct FPostHogClient::FImpl : public TSharedFromThis<FImpl, ESPMode::ThreadSafe>
{
    FCriticalSection Mutex;
    FPostHogConfig Config;
    FString StateFile;
    phu_handle Handle = 0;
    bool bClosed = false;
    bool bOptedOut = false;
    TOptional<bool> DesiredOptOut;
    bool bIdentified = false;
    bool bPersonProcessing = false;
    uint64 ConsentGeneration = 0;
    uint64 IdentityGeneration = 0;
    FString AnonymousId;
    FString DistinctId;
    FPostHogProperties Registered;
    FPostHogProperties Groups;
    FPostHogProperties LastSet;
    FPostHogProperties LastSetOnce;
    bool bHasPersonProperties = false;
    FString SessionId;
    FDateTime SessionStarted;
    FDateTime LastActivity;
    bool bForeground = true;
    bool bLifecycleStarted = false;
    FString LastAppVersion;
    FString LastAppBuild;
    bool bWriting = false;
    FString PendingState;
    TOptional<uint64> PendingCacheRevision;
    TOptional<uint64> SavedCacheRevision;
    TSharedFuture<bool> WriteFuture;
    TSharedFuture<bool> CloseFuture;
    int64 StateWriteErrors = 0;
    FPostHogProperties PersonFlagProperties;
    FPostHogProperties GroupFlagProperties;
    FPostHogFlagCache FlagCache;
    uint64 FlagGeneration = 0;
    uint64 LoadingFlagGeneration = 0;
    uint64 CacheFlagGeneration = 0;
    bool bLoadingFlags = false;
    bool bReloadFlagsRequested = false;
    TSharedFuture<FPostHogFlagReloadResult> FlagFuture;

    FImpl(FPostHogConfig InConfig, FString InStateFile) : Config(MoveTemp(InConfig)), StateFile(MoveTemp(InStateFile))
    {
        bOptedOut = Config.bInitiallyOptedOut;
        AnonymousId = PostHogJson::NewUuidV7(Config.Now());
        DistinctId = AnonymousId;
    }

    void Restore()
    {
        auto& Files = FPlatformFileManager::Get().GetPlatformFile();
        if (!Files.FileExists(*StateFile)) { return; }
        FString Json;
        if (Files.FileSize(*StateFile) > 1024 * 1024 || !FFileHelper::LoadFileToString(Json, *StateFile))
        {
            UE_LOG(LogPostHogClient, Warning, TEXT("Could not read PostHog client state.")); return;
        }
        auto State = PostHogJson::Decode(Json);
        double Version = 0;
        FString Anonymous, Distinct;
        bool OptedOut = false, Identified = false;
        if (!State || !State->TryGetNumberField(TEXT("version"), Version) || Version != 1 ||
            !State->TryGetStringField(TEXT("anonymous_id"), Anonymous) || !NonBlank(Anonymous) ||
            !State->TryGetStringField(TEXT("distinct_id"), Distinct) || !NonBlank(Distinct) ||
            !State->TryGetBoolField(TEXT("opted_out"), OptedOut) || !State->TryGetBoolField(TEXT("identified"), Identified))
        {
            UE_LOG(LogPostHogClient, Warning, TEXT("Ignoring invalid PostHog client state.")); return;
        }
        AnonymousId = Anonymous; DistinctId = Distinct; bOptedOut = OptedOut; bIdentified = Identified;
        State->TryGetBoolField(TEXT("person_processing"), bPersonProcessing);
        State->TryGetStringField(TEXT("app_version"), LastAppVersion);
        State->TryGetStringField(TEXT("app_build"), LastAppBuild);
        const TSharedPtr<FJsonObject>* Object = nullptr;
        if (State->TryGetObjectField(TEXT("registered"), Object)) { Registered = FPostHogProperties::FromNative(Object->ToSharedRef()); }
        if (State->TryGetObjectField(TEXT("groups"), Object)) { Groups = FPostHogProperties::FromNative(Object->ToSharedRef()); }
        if (State->TryGetObjectField(TEXT("person_flag_properties"), Object)) { PersonFlagProperties = FPostHogProperties::FromNative(Object->ToSharedRef()); }
        if (State->TryGetObjectField(TEXT("group_flag_properties"), Object)) { GroupFlagProperties = FPostHogProperties::FromNative(Object->ToSharedRef()); }
        const TSharedPtr<FJsonObject>* Context = nullptr;
        if (!bOptedOut && State->TryGetObjectField(TEXT("flag_context"), Context) &&
            SameProperties(FPostHogProperties::FromNative(Context->ToSharedRef()), FlagContextLocked()) &&
            State->TryGetObjectField(TEXT("cached_flags"), Object) &&
            FlagCache.Restore(FPostHogProperties::FromNative(Object->ToSharedRef())) &&
            State->TryGetObjectField(TEXT("cached_session_recording"), Object))
        { FlagCache.RestoreSessionRecording(*Object); CacheFlagGeneration = FlagGeneration; }
    }

    FPostHogProperties FlagContextLocked() const
    {
        auto Context = MakeShared<FJsonObject>();
        Context->SetStringField(TEXT("distinct_id"), DistinctId);
        Context->SetObjectField(TEXT("groups"), Groups.ToNative());
        Context->SetObjectField(TEXT("person_properties"), PersonFlagProperties.ToNative());
        Context->SetObjectField(TEXT("group_properties"), GroupFlagProperties.ToNative());
        return FPostHogProperties::FromNative(Context);
    }

    FString StateJsonLocked(TOptional<bool> ConsentOverride = {}, bool bIncludeFlags = true, bool* CacheIncluded = nullptr) const
    {
        auto State = MakeShared<FJsonObject>();
        State->SetNumberField(TEXT("version"), 1);
        State->SetStringField(TEXT("anonymous_id"), AnonymousId);
        State->SetStringField(TEXT("distinct_id"), DistinctId);
        State->SetBoolField(TEXT("identified"), bIdentified);
        State->SetBoolField(TEXT("person_processing"), bPersonProcessing);
        State->SetStringField(TEXT("app_version"), LastAppVersion);
        State->SetStringField(TEXT("app_build"), LastAppBuild);
        State->SetBoolField(TEXT("opted_out"), ConsentOverride.IsSet() ? ConsentOverride.GetValue() : DesiredOptOut.Get(bOptedOut));
        State->SetObjectField(TEXT("registered"), Registered.ToNative());
        State->SetObjectField(TEXT("groups"), Groups.ToNative());
        State->SetObjectField(TEXT("person_flag_properties"), PersonFlagProperties.ToNative());
        State->SetObjectField(TEXT("group_flag_properties"), GroupFlagProperties.ToNative());
        if (CacheIncluded) { *CacheIncluded = false; }
        if (bIncludeFlags && FlagCache.bLoaded && !bOptedOut && CacheFlagGeneration == FlagGeneration)
        {
            State->SetObjectField(TEXT("flag_context"), FlagContextLocked().ToNative());
            State->SetObjectField(TEXT("cached_flags"), FlagCache.Serialize().ToNative());
            if (FlagCache.SessionRecording.IsSet())
            { State->SetObjectField(TEXT("cached_session_recording"), FlagCache.SessionRecording->ToNative()); }
            const FString WithCache = PostHogJson::Encode(State);
            if (PostHogJson::IsBounded(WithCache) && FTCHARToUTF8(*WithCache).Length() <= 1024 * 1024 - 1024)
            { if (CacheIncluded) { *CacheIncluded = true; } return WithCache; }
            // Optional cache data must never prevent identity/consent persistence.
            State->RemoveField(TEXT("flag_context")); State->RemoveField(TEXT("cached_flags")); State->RemoveField(TEXT("cached_session_recording"));
        }
        return PostHogJson::Encode(State);
    }

    bool CanPersistLocked(TOptional<FString> ProposedDistinctId = {}) const
    {
        auto State = PostHogJson::Decode(StateJsonLocked(false, false));
        if (!State) { return false; }
        // Reserve space for a subsequent anonymous reset and boolean changes.
        State->SetBoolField(TEXT("identified"), false);
        State->SetBoolField(TEXT("person_processing"), false);
        const FString Id = ProposedDistinctId.Get(DistinctId);
        State->SetStringField(TEXT("distinct_id"), Id.Len() < 36 ? FString::ChrN(36, TEXT('0')) : Id);
        const FString Json = PostHogJson::Encode(State.ToSharedRef());
        return PostHogJson::IsBounded(Json) && FTCHARToUTF8(*Json).Length() <= 1024 * 1024 - 1024;
    }

    TSharedFuture<bool> PersistLocked()
    {
        if (!CanPersistLocked()) { ++StateWriteErrors; return Ready(false); }
        bool bCacheIncluded = false;
        PendingState = StateJsonLocked({}, true, &bCacheIncluded);
        PendingCacheRevision = bCacheIncluded ? TOptional<uint64>(FlagCache.Revision) : TOptional<uint64>();
        if (bWriting) { return WriteFuture; }
        bWriting = true;
        auto Promise = MakeShared<TPromise<bool>, ESPMode::ThreadSafe>();
        WriteFuture = Promise->GetFuture().Share();
        auto Self = AsShared();
        if (!PostHogTasks::Launch([Self, Promise]
        {
            bool bSuccess = true;
            for (;;)
            {
                FString Json;
                TOptional<uint64> CacheRevision;
                { FScopeLock Lock(&Self->Mutex); Json = MoveTemp(Self->PendingState); Self->PendingState.Empty(); CacheRevision = Self->PendingCacheRevision; }
                const bool bSaved = SaveAtomic(Self->StateFile, Json);
                bSuccess &= bSaved;
                bool bDone;
                {
                    FScopeLock Lock(&Self->Mutex);
                    if (!bSaved) { ++Self->StateWriteErrors; }
                    else { Self->SavedCacheRevision = CacheRevision; }
                    bDone = Self->PendingState.IsEmpty();
                    if (bDone) { Self->bWriting = false; }
                }
                if (!bSaved) { UE_LOG(LogPostHogClient, Warning, TEXT("Could not persist PostHog client state.")); }
                if (bDone) { break; }
            }
            Promise->SetValue(bSuccess);
        }))
        {
            bWriting = false; ++StateWriteErrors; Promise->SetValue(false);
        }
        return WriteFuture;
    }

    void StartSessionLocked(const FDateTime& Now)
    {
        SessionId = PostHogJson::NewUuidV7(Now); SessionStarted = Now; LastActivity = Now;
    }
    FString SessionLocked(const FDateTime& Now, bool bActivity)
    {
        if (Config.SessionMode == EPostHogSessionMode::Automatic)
        {
            if (!SessionId.IsEmpty() && (Now - LastActivity > FTimespan::FromMinutes(30) || Now - SessionStarted > FTimespan::FromHours(24)))
            {
                SessionId.Empty();
            }
            if (SessionId.IsEmpty() && bForeground) { StartSessionLocked(Now); }
        }
        if (bActivity && !SessionId.IsEmpty()) { LastActivity = Now; }
        return SessionId;
    }
    bool CanCaptureLocked() const { return !bClosed && Handle != 0 && !bOptedOut; }
    bool CanProcessPeopleLocked() const { return CanCaptureLocked() && Config.PersonProfiles != EPostHogPersonProfiles::Never; }

    FPostHogEvent SnapshotLocked(const FString& Name, const FPostHogProperties& Properties)
    {
        FPostHogEvent Event;
        Event.Event = Name; Event.DistinctId = DistinctId; Event.Timestamp = Config.Now();
        Event.ConsentGeneration = ConsentGeneration;
        Event.Uuid = PostHogJson::NewUuidV7(Event.Timestamp);
        auto Merged = Registered.ToNative();
        PostHogJson::Merge(*Merged, *Config.Context.ToNative());
        Merged->SetStringField(TEXT("$lib"), PostHogJson::LibName);
        Merged->SetStringField(TEXT("$lib_version"), PostHogJson::LibVersion);
        if (!Groups.IsEmpty()) { Merged->SetObjectField(TEXT("$groups"), Groups.ToNative()); }
        TArray<TSharedPtr<FJsonValue>> ActiveFlags;
        for (const auto& Pair : FlagCache.Flags)
        {
            const auto Value = Pair.Value.Value.ToNative();
            Merged->SetField(TEXT("$feature/") + Pair.Key, Value);
            if (Value->Type == EJson::String || (Value->Type == EJson::Boolean && Value->AsBool()))
            { ActiveFlags.Add(MakeShared<FJsonValueString>(Pair.Key)); }
        }
        if (FlagCache.bLoaded) { Merged->SetArrayField(TEXT("$active_feature_flags"), ActiveFlags); }
        PostHogJson::Merge(*Merged, *Properties.ToNative());
        const bool bProcess = Config.PersonProfiles != EPostHogPersonProfiles::Never &&
            (Config.PersonProfiles == EPostHogPersonProfiles::Always || bIdentified || bPersonProcessing);
        Merged->SetBoolField(TEXT("$process_person_profile"), bProcess);
        Merged->SetBoolField(TEXT("$is_identified"), bIdentified);
        const FString Session = SessionLocked(Event.Timestamp, true);
        if (!Session.IsEmpty()) { Merged->SetStringField(TEXT("$session_id"), Session); }
        else { Merged->RemoveField(TEXT("$session_id")); }
        PostHogJson::RemoveNullObjectProperties(MakeShared<FJsonValueObject>(Merged));
        Event.Properties = FPostHogProperties::FromNative(Merged);
        return Event;
    }

    EPostHogCaptureResult Emit(FPostHogEvent Event, bool bMinimal = false)
    {
        const uint64 AdmissionGeneration = Event.ConsentGeneration;
        // Config is immutable after creation. Hooks can safely reenter this client.
        if (Config.BeforeSend && !Config.BeforeSend(Event)) { return EPostHogCaptureResult::Disabled; }
        if (!NonBlank(Event.Event) || !NonBlank(Event.DistinctId) || !Event.Properties.IsValid()) { return EPostHogCaptureResult::Invalid; }
        if (bMinimal) { FPostHogFlagCache::Minimize(Event.Properties); }
        auto Envelope = MakeShared<FJsonObject>();
        Envelope->SetStringField(TEXT("event"), Event.Event);
        Envelope->SetStringField(TEXT("distinct_id"), Event.DistinctId);
        Envelope->SetStringField(TEXT("uuid"), Event.Uuid);
        Envelope->SetStringField(TEXT("timestamp"), Event.Timestamp.ToIso8601());
        Envelope->SetObjectField(TEXT("properties"), Event.Properties.ToNative());
        const FString Json = PostHogJson::Encode(Envelope);
        if (!PostHogJson::IsBounded(Json)) { return EPostHogCaptureResult::Invalid; }
        const FTCHARToUTF8 Utf8(*Json);
        FScopeLock Lock(&Mutex);
        if (bClosed || Handle == 0) { return EPostHogCaptureResult::Closed; }
        if (bOptedOut || ConsentGeneration != AdmissionGeneration) { return EPostHogCaptureResult::Disabled; }
        return FromNative(phu_capture(Handle, View(Utf8)));
    }

    void InvalidateFlagsLocked()
    {
        ++FlagGeneration;
        FlagCache.Clear();
    }

    void StaleFlagsLocked() { ++FlagGeneration; }

    TSharedFuture<FPostHogFlagReloadResult> ReloadFlagsLocked()
    {
        if (!CanCaptureLocked())
        { return Ready(FPostHogFlagReloadResult{bClosed ? EPostHogFlagReloadStatus::Closed : EPostHogFlagReloadStatus::Disabled, false}); }
        if (bLoadingFlags)
        {
            bReloadFlagsRequested |= LoadingFlagGeneration != FlagGeneration;
            return FlagFuture;
        }
        bLoadingFlags = true;
        LoadingFlagGeneration = FlagGeneration;
        auto Self = AsShared();
        FlagFuture = PostHogTasks::Run([Self]
        {
            for (;;)
            {
                FString Context;
                uint64 Generation, Consent;
                phu_handle NativeHandle;
                {
                    FScopeLock Lock(&Self->Mutex);
                    if (!Self->CanCaptureLocked())
                    {
                        Self->bLoadingFlags = false;
                        return FPostHogFlagReloadResult{Self->bClosed ? EPostHogFlagReloadStatus::Closed : EPostHogFlagReloadStatus::Disabled, false};
                    }
                    Generation = Self->FlagGeneration; Consent = Self->ConsentGeneration;
                    Self->LoadingFlagGeneration = Generation; Self->bReloadFlagsRequested = false;
                    Context = Self->FlagContextLocked().ToJson(); NativeHandle = Self->Handle;
                }
                const FTCHARToUTF8 Utf8(*Context);
                phu_buffer Buffer{nullptr, 0};
                const phu_status Status = phu_flags(NativeHandle, View(Utf8), &Buffer);
                TSharedPtr<FJsonObject> Response;
                if (Status == PHU_OK && Buffer.data && Buffer.len <= 1024 * 1024)
                {
                    const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Buffer.data), static_cast<int32>(Buffer.len));
                    Response = PostHogJson::Decode(FString(Text.Length(), Text.Get()));
                }
                phu_buffer_free(Buffer);
                FPostHogFlagReloadResult Result;
                TSharedFuture<bool> Saved;
                uint64 CacheRevision = 0;
                {
                    FScopeLock Lock(&Self->Mutex);
                    if (!Self->bClosed && !Self->bOptedOut && Self->FlagGeneration == Generation && Self->ConsentGeneration == Consent && Response)
                    {
                        Result.Status = Self->FlagCache.Apply(Response.ToSharedRef());
                        if (Result.Status == EPostHogFlagReloadStatus::Success || Result.Status == EPostHogFlagReloadStatus::Partial)
                        {
                            Self->CacheFlagGeneration = Generation;
                            CacheRevision = Self->FlagCache.Revision;
                            Saved = Self->PersistLocked();
                        }
                    }
                }
                const bool bSaved = Saved.IsValid() && Saved.Get();
                {
                    FScopeLock Lock(&Self->Mutex);
                    Result.bPersisted = bSaved && Self->SavedCacheRevision.IsSet() && Self->SavedCacheRevision.GetValue() == CacheRevision;
                    if (Self->bClosed) { Result = {EPostHogFlagReloadStatus::Closed, false}; }
                    else if (Self->bOptedOut) { Result = {EPostHogFlagReloadStatus::Disabled, false}; }
                    else if (Self->FlagGeneration != Generation || Self->ConsentGeneration != Consent)
                    { Result = {EPostHogFlagReloadStatus::Stale, false}; }
                    if (Self->CanCaptureLocked() && Self->bReloadFlagsRequested) { continue; }
                    if (Self->CanCaptureLocked() && Self->FlagGeneration == Generation && Self->ConsentGeneration == Consent)
                    { Result.Flags = Self->FlagCache.Values(); }
                    Self->bLoadingFlags = false;
                    return Result;
                }
            }
        });
        // Run returns a ready default result if native thread creation failed.
        if (FlagFuture.IsReady()) { bLoadingFlags = false; }
        return FlagFuture;
    }

    TSharedFuture<bool> OptInLocked()
    {
        const uint64 Generation = ++ConsentGeneration;
        ++FlagGeneration;
        const phu_handle NativeHandle = Handle;
        auto Self = AsShared();
        return PostHogTasks::Run([Self, Generation, NativeHandle]
        {
            { FScopeLock Lock(&Self->Mutex); if (Self->bClosed || Self->ConsentGeneration != Generation) { return false; } }
            // Keep persisted opt-out until the old spool has been purged. The
            // local gate remains closed while native admission resumes.
            const phu_status Status = phu_opt_in(NativeHandle);
            TSharedFuture<bool> Saved;
            {
                FScopeLock Lock(&Self->Mutex);
                if (Self->bClosed || Self->ConsentGeneration != Generation || Status != PHU_OK) { return false; }
                Self->DesiredOptOut = false;
                Saved = Self->PersistLocked();
            }
            const bool bSaved = Saved.Get();
            FScopeLock Lock(&Self->Mutex);
            if (Self->bClosed || Self->ConsentGeneration != Generation) { return false; }
            if (!bSaved)
            {
                Self->DesiredOptOut = Self->bOptedOut;
                if (Self->bOptedOut) { phu_opt_out(NativeHandle); }
                Self->PersistLocked();
                return false;
            }
            Self->bOptedOut = false;
            Self->ReloadFlagsLocked();
            return true;
        });
    }

    TSharedFuture<bool> Close()
    {
        FScopeLock Lock(&Mutex);
        if (CloseFuture.IsValid()) { return CloseFuture; }
        const bool bCacheCurrent = CacheFlagGeneration == FlagGeneration;
        bClosed = true; ++ConsentGeneration; ++FlagGeneration; FlagCache.Reported.Empty(); DesiredOptOut = bOptedOut;
        if (bCacheCurrent) { CacheFlagGeneration = FlagGeneration; }
        if (Handle != 0) { phu_begin_shutdown(Handle); }
        const auto Saved = PersistLocked();
        const phu_handle NativeHandle = Handle;
        Handle = 0;
        auto Self = AsShared();
        CloseFuture = PostHogTasks::Run([Self, Saved, NativeHandle]
        {
            const bool bSaved = Saved.Get();
            const phu_status Status = NativeHandle == 0 ? PHU_OK : phu_close(NativeHandle);
            return bSaved && (Status == PHU_OK || Status == PHU_CLOSED);
        });
        return CloseFuture;
    }
};

TSharedFuture<FPostHogClient::FPtr> FPostHogClient::CreateAsync(FPostHogConfig Config)
{
    if (!NonBlank(Config.ProjectToken) || !NonBlank(Config.Host) || Config.StorageRoot.IsEmpty() || !NonBlank(Config.ClientName) || !Config.Now)
    { return Ready(FPtr()); }
    FScopeLock AdmissionLock(&ClientsMutex);
    if (bStopping) { return Ready(FPtr()); }
    return PostHogTasks::Run([Config = MoveTemp(Config)]() mutable -> FPtr
    {
        Config.ProjectToken.TrimStartAndEndInline(); Config.Host.TrimStartAndEndInline();
        while (Config.Host.EndsWith(TEXT("/"))) { Config.Host.LeftChopInline(1); }
        const FString Namespace = FMD5::HashAnsiString(*(Config.Host + TEXT("\n") + Config.ProjectToken + TEXT("\n") + Config.ClientName));
        const FString Directory = FPaths::ConvertRelativePathToFull(Config.StorageRoot / Namespace);
        auto State = MakeShared<FImpl, ESPMode::ThreadSafe>(MoveTemp(Config), Directory / TEXT("state.json"));
        State->Restore();
        const auto& Options = State->Config;
        auto Json = MakeShared<FJsonObject>();
        Json->SetStringField(TEXT("project_token"), Options.ProjectToken);
        Json->SetStringField(TEXT("host"), Options.Host);
        Json->SetStringField(TEXT("storage_path"), Directory / TEXT("events"));
        Json->SetBoolField(TEXT("opted_out"), State->bOptedOut);
        Json->SetNumberField(TEXT("flush_at"), Options.FlushAt);
        Json->SetNumberField(TEXT("flush_interval_ms"), Options.FlushIntervalMilliseconds);
        Json->SetNumberField(TEXT("max_queue_size"), Options.MaxQueueSize);
        Json->SetNumberField(TEXT("max_batch_size"), Options.MaxBatchSize);
        Json->SetNumberField(TEXT("request_timeout_seconds"), Options.RequestTimeoutSeconds);
        Json->SetNumberField(TEXT("shutdown_timeout_ms"), Options.ShutdownTimeoutMilliseconds);
        // Native owns the replay queue defaults; the object's presence enables replay.
        if (Options.Replay.bEnabled) { Json->SetObjectField(TEXT("replay"), MakeShared<FJsonObject>()); }
        const FTCHARToUTF8 Utf8(*PostHogJson::Encode(Json));
        if (phu_abi_version() != PHU_ABI_VERSION || phu_create(PHU_ABI_VERSION, View(Utf8), &State->Handle) != PHU_OK) { return nullptr; }
        // First identity must be saved before initialization becomes observable.
        if (!SaveAtomic(State->StateFile, State->StateJsonLocked()))
        { ++State->StateWriteErrors; UE_LOG(LogPostHogClient, Warning, TEXT("PostHog identity persistence is unavailable.")); }
        {
            FScopeLock Lock(&ClientsMutex);
            if (bStopping) { phu_close(State->Handle); return nullptr; }
            Clients.RemoveAll([](const auto& Client) { return !Client.IsValid(); });
            Clients.Add(State);
            if (State->Config.bPreloadFeatureFlags) { FScopeLock StateLock(&State->Mutex); State->ReloadFlagsLocked(); }
        }
        return FPtr(new FPostHogClient(State));
    });
}

FPostHogClient::~FPostHogClient() { Impl->Close(); }
bool FPostHogClient::IsInitialized() const { FScopeLock Lock(&Impl->Mutex); return !Impl->bClosed && Impl->Handle != 0; }
FString FPostHogClient::GetDistinctId() const { FScopeLock Lock(&Impl->Mutex); return Impl->DistinctId; }
FString FPostHogClient::GetAnonymousId() const { FScopeLock Lock(&Impl->Mutex); return Impl->AnonymousId; }
bool FPostHogClient::IsOptedOut() const { FScopeLock Lock(&Impl->Mutex); return Impl->bOptedOut; }

EPostHogCaptureResult FPostHogClient::Capture(const FString& Name, const FPostHogProperties& Properties)
{
    if (!NonBlank(Name) || !Properties.IsValid()) { return EPostHogCaptureResult::Invalid; }
    FPostHogEvent Event;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bClosed) { return EPostHogCaptureResult::Closed; }
        if (!Impl->CanCaptureLocked()) { return EPostHogCaptureResult::Disabled; }
        Event = Impl->SnapshotLocked(Name, Properties);
    }
    return Impl->Emit(MoveTemp(Event));
}
EPostHogCaptureResult FPostHogClient::Screen(const FString& Name, const FPostHogProperties& Properties)
{
    if (!NonBlank(Name)) { return EPostHogCaptureResult::Invalid; }
    auto Values = Properties; Values.SetString(TEXT("$screen_name"), Name);
    return Capture(TEXT("$screen"), Values);
}

bool FPostHogClient::Identify(const FString& Id, const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if (!NonBlank(Id) || !Set.IsValid() || !SetOnce.IsValid())
    { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored identify: invalid distinct id or properties.")); return false; }
    FPostHogEvent Event;
    uint64 Generation = 0;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (!Impl->CanProcessPeopleLocked()) { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored identify: person processing is unavailable.")); return false; }
        if (Impl->bIdentified && Impl->DistinctId != Id) { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored identify: already identified as a different user.")); return false; }
        if (!Impl->CanPersistLocked(Id)) { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored identify: client state is too large to persist.")); return false; }
        const bool bWasIdentified = Impl->bIdentified;
        if (bWasIdentified && ((Set.IsEmpty() && SetOnce.IsEmpty()) || (Impl->bHasPersonProperties && SameProperties(Set, Impl->LastSet) && SameProperties(SetOnce, Impl->LastSetOnce))))
        { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored identify: duplicate of the current identity.")); return false; }
        auto Properties = MakeShared<FJsonObject>();
        const bool bLink = !bWasIdentified && Id != Impl->DistinctId;
        if (bLink) { Properties->SetStringField(TEXT("$anon_distinct_id"), Impl->DistinctId); }
        Properties->SetObjectField(TEXT("$set"), Set.ToNative());
        Properties->SetObjectField(TEXT("$set_once"), SetOnce.ToNative());
        const bool bReload = !bWasIdentified && (bLink || !Set.IsEmpty() || !SetOnce.IsEmpty());
        Impl->DistinctId = Id; Impl->bIdentified = true;
        if (!bWasIdentified) { ++Impl->IdentityGeneration; }
        if (bLink) { Impl->FlagCache.Reported.Empty(); }
        if (bReload) { Impl->StaleFlagsLocked(); }
        Generation = Impl->IdentityGeneration;
        Event = Impl->SnapshotLocked(bLink ? TEXT("$identify") : TEXT("$set"), FPostHogProperties::FromNative(Properties));
        Impl->PersistLocked();
        if (bReload) { Impl->ReloadFlagsLocked(); }
    }
    if (Impl->Emit(MoveTemp(Event)) == EPostHogCaptureResult::Accepted)
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->IdentityGeneration == Generation && !Impl->bClosed)
        { Impl->LastSet = Set; Impl->LastSetOnce = SetOnce; Impl->bHasPersonProperties = true; }
    }
    return true;
}

TSharedFuture<bool> FPostHogClient::ResetAsync()
{
    FScopeLock Lock(&Impl->Mutex);
    if (Impl->bClosed) { return Ready(false); }
    ++Impl->IdentityGeneration;
    Impl->AnonymousId = PostHogJson::NewUuidV7(Impl->Config.Now()); Impl->DistinctId = Impl->AnonymousId;
    Impl->bIdentified = false; Impl->bPersonProcessing = false; Impl->bHasPersonProperties = false;
    Impl->Registered = {}; Impl->Groups = {}; Impl->LastSet = {}; Impl->LastSetOnce = {};
    Impl->PersonFlagProperties = {}; Impl->GroupFlagProperties = {}; Impl->InvalidateFlagsLocked();
    if (Impl->Config.SessionMode == EPostHogSessionMode::Automatic || !Impl->SessionId.IsEmpty()) { Impl->StartSessionLocked(Impl->Config.Now()); }
    if (Impl->bOptedOut) { return Impl->OptInLocked(); }
    Impl->ReloadFlagsLocked();
    return Impl->PersistLocked();
}

void FPostHogClient::Alias(const FString& AliasId)
{
    if (!NonBlank(AliasId)) { return; }
    FPostHogEvent Event;
    { FScopeLock Lock(&Impl->Mutex); if (!Impl->CanProcessPeopleLocked()) { return; }
        Impl->bPersonProcessing = true; Impl->PersistLocked();
        FPostHogProperties Values; Values.SetString(TEXT("alias"), AliasId); Values.SetString(TEXT("distinct_id"), Impl->DistinctId);
        Event = Impl->SnapshotLocked(TEXT("$create_alias"), Values); }
    Impl->Emit(MoveTemp(Event));
}
void FPostHogClient::Register(const FPostHogProperties& Properties)
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return; }
    auto Values = Impl->Registered.ToNative(); PostHogJson::Merge(*Values, *Properties.ToNative());
    auto Candidate = FPostHogProperties::FromNative(Values);
    if (!Candidate.IsValid()) { return; }
    auto Previous = Impl->Registered;
    Impl->Registered = MoveTemp(Candidate);
    if (!Impl->CanPersistLocked()) { Impl->Registered = MoveTemp(Previous); return; }
    Impl->PersistLocked();
}
void FPostHogClient::Unregister(const FString& Key)
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return; }
    Impl->Registered.Remove(Key); Impl->PersistLocked();
}
void FPostHogClient::SetPersonProperties(const FPostHogProperties& Set, const FPostHogProperties& SetOnce)
{
    if ((Set.IsEmpty() && SetOnce.IsEmpty()) || !Set.IsValid() || !SetOnce.IsValid()) { return; }
    FPostHogEvent Event;
    uint64 Generation = 0;
    { FScopeLock Lock(&Impl->Mutex); if (!Impl->CanProcessPeopleLocked()) { return; }
        if (Impl->bHasPersonProperties && SameProperties(Set, Impl->LastSet) && SameProperties(SetOnce, Impl->LastSetOnce)) { return; }
        Impl->bPersonProcessing = true;
        FPostHogProperties Properties; Properties.Set(TEXT("$set"), FPostHogValue::Object(Set)); Properties.Set(TEXT("$set_once"), FPostHogValue::Object(SetOnce));
        Event = Impl->SnapshotLocked(TEXT("$set"), Properties);
        Generation = Impl->IdentityGeneration; Impl->PersistLocked(); }
    if (Impl->Emit(MoveTemp(Event)) == EPostHogCaptureResult::Accepted)
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->IdentityGeneration == Generation && !Impl->bClosed)
        { Impl->LastSet = Set; Impl->LastSetOnce = SetOnce; Impl->bHasPersonProperties = true; }
    }
}
void FPostHogClient::Group(const FString& Type, const FString& Key, const FPostHogProperties& Properties)
{
    if (!NonBlank(Type) || !NonBlank(Key)) { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored group: blank group type or key.")); return; }
    FPostHogEvent Event;
    { FScopeLock Lock(&Impl->Mutex);
        if (!Impl->CanProcessPeopleLocked()) { UE_LOG(LogPostHogClient, Warning, TEXT("Ignored group: person processing is unavailable.")); return; }
        auto Previous = Impl->Groups;
        if (!Impl->Groups.SetString(Type, Key)) { return; }
        if (!Impl->CanPersistLocked()) { Impl->Groups = MoveTemp(Previous); return; }
        if (!SameProperties(Previous, Impl->Groups))
        { Impl->GroupFlagProperties.Remove(Type); Impl->StaleFlagsLocked(); Impl->ReloadFlagsLocked(); }
        Impl->bPersonProcessing = true; Impl->PersistLocked();
        FPostHogProperties Values; Values.SetString(TEXT("$group_type"), Type); Values.SetString(TEXT("$group_key"), Key); Values.Set(TEXT("$group_set"), FPostHogValue::Object(Properties));
        Event = Impl->SnapshotLocked(TEXT("$groupidentify"), Values); }
    Impl->Emit(MoveTemp(Event));
}

TSharedFuture<bool> FPostHogClient::OptOutAsync()
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return Ready(false); }
    Impl->bOptedOut = true; Impl->DesiredOptOut = true; ++Impl->ConsentGeneration;
    Impl->InvalidateFlagsLocked();
    phu_opt_out(Impl->Handle);
    const auto Saved = Impl->PersistLocked();
    const auto State = Impl; const phu_handle Handle = Impl->Handle;
    return PostHogTasks::Run([State, Saved, Handle]
    {
        const bool bSaved = Saved.Get();
        phu_buffer Report{nullptr, 0}; const phu_status Status = phu_checkpoint(Handle, &Report);
        bool bPurged = false;
        if (Status == PHU_OK && Report.data)
        {
            const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Report.data), static_cast<int32>(Report.len));
            auto Json = PostHogJson::Decode(FString(Text.Length(), Text.Get())); double Errors = 0;
            bPurged = Json && Json->TryGetNumberField(TEXT("persistence_errors"), Errors) && Errors == 0;
        }
        phu_buffer_free(Report);
        return bSaved && bPurged;
    });
}
TSharedFuture<bool> FPostHogClient::OptInAsync()
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return Ready(false); }
    return Impl->OptInLocked();
}
TSharedFuture<FPostHogDeliveryResult> FPostHogClient::FlushAsync()
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return Ready(FPostHogDeliveryResult()); }
    const auto State = Impl; const phu_handle Handle = Impl->Handle;
    const auto Saved = Impl->PersistLocked();
    return PostHogTasks::Run([State, Handle, Saved]
    {
        Saved.Wait(); FPostHogDeliveryResult Result;
        if (phu_flush(Handle) != PHU_OK) { return Result; }
        phu_buffer Report{nullptr, 0};
        if (phu_checkpoint(Handle, &Report) == PHU_OK && Report.data)
        {
            const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Report.data), static_cast<int32>(Report.len));
            auto Json = PostHogJson::Decode(FString(Text.Length(), Text.Get()));
            if (Json)
            {
                Result.bCompleted = true;
                Json->TryGetNumberField(TEXT("pending"), Result.Pending);
                Json->TryGetNumberField(TEXT("delivered"), Result.Delivered);
                Json->TryGetNumberField(TEXT("terminally_dropped"), Result.TerminallyDropped);
                Json->TryGetNumberField(TEXT("persistence_errors"), Result.StorageErrors);
                FScopeLock StateLock(&State->Mutex); Result.StorageErrors += State->StateWriteErrors;
            }
        }
        phu_buffer_free(Report); return Result;
    });
}
TSharedFuture<bool> FPostHogClient::ShutdownAsync() { return Impl->Close(); }
FPostHogFeatureFlagResult FPostHogClient::GetFeatureFlagResult(const FString& Key, bool bSendEvent)
{
    FPostHogFeatureFlagResult Result;
    FPostHogEvent Event;
    bool bTrack = false, bMinimal = false;
    uint64 Revision = 0;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (!Impl->CanCaptureLocked()) { return Result; }
        const auto* Found = Impl->FlagCache.Flags.Find(Key);
        if (!Found) { return Result; }
        Result = *Found;
        if (bSendEvent && Impl->Config.bSendFeatureFlagEvents && !Impl->FlagCache.Reported.Contains(Key))
        {
            Impl->FlagCache.Reported.Add(Key); Revision = Impl->FlagCache.Revision;
            Event = Impl->SnapshotLocked(TEXT("$feature_flag_called"), FPostHogFlagCache::Exposure(Key, Result));
            bMinimal = FPostHogFlagCache::IsMinimal(Result); bTrack = true;
        }
    }
    if (bTrack && Impl->Emit(MoveTemp(Event), bMinimal) != EPostHogCaptureResult::Accepted)
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->FlagCache.Revision == Revision) { Impl->FlagCache.Reported.Remove(Key); }
    }
    return Result;
}
FPostHogValue FPostHogClient::GetFeatureFlag(const FString& Key, bool bSendEvent)
{ return GetFeatureFlagResult(Key, bSendEvent).Value; }
bool FPostHogClient::IsFeatureEnabled(const FString& Key, bool bSendEvent, bool bDefaultValue)
{
    const auto Result = GetFeatureFlagResult(Key, bSendEvent);
    if (!Result.bFound) { return bDefaultValue; }
    const auto Value = Result.Value.ToNative();
    return Value->Type == EJson::String || (Value->Type == EJson::Boolean && Value->AsBool());
}
FPostHogValue FPostHogClient::GetFeatureFlagPayload(const FString& Key)
{ return GetFeatureFlagResult(Key, false).Payload; }
FPostHogProperties FPostHogClient::GetFeatureFlags() const
{
    FScopeLock Lock(&Impl->Mutex);
    return Impl->CanCaptureLocked() ? Impl->FlagCache.Values() : FPostHogProperties();
}
bool FPostHogClient::HasLoadedFeatureFlags() const
{ FScopeLock Lock(&Impl->Mutex); return Impl->CanCaptureLocked() && Impl->FlagCache.bLoaded; }
TSharedFuture<FPostHogFlagReloadResult> FPostHogClient::ReloadFeatureFlagsAsync()
{ FScopeLock Lock(&Impl->Mutex); return Impl->ReloadFlagsLocked(); }
bool FPostHogClient::SetPersonPropertiesForFlags(const FPostHogProperties& Properties, bool bReload)
{
    FScopeLock Lock(&Impl->Mutex);
    if (!Impl->CanCaptureLocked() || !Properties.IsValid() || Properties.IsEmpty()) { return false; }
    auto Object = Impl->PersonFlagProperties.ToNative(); PostHogJson::Merge(*Object, *Properties.ToNative());
    auto Candidate = FPostHogProperties::FromNative(Object);
    if (!Candidate.IsValid()) { return false; }
    if (SameProperties(Candidate, Impl->PersonFlagProperties))
    { if (bReload) { Impl->ReloadFlagsLocked(); } return true; }
    auto Previous = Impl->PersonFlagProperties; Impl->PersonFlagProperties = MoveTemp(Candidate);
    if (!Impl->CanPersistLocked()) { Impl->PersonFlagProperties = MoveTemp(Previous); return false; }
    Impl->StaleFlagsLocked(); Impl->PersistLocked();
    if (bReload) { Impl->ReloadFlagsLocked(); }
    return true;
}
bool FPostHogClient::SetGroupPropertiesForFlags(const FString& Type, const FPostHogProperties& Properties, bool bReload)
{
    FScopeLock Lock(&Impl->Mutex);
    if (!Impl->CanCaptureLocked() || !NonBlank(Type) || !Properties.IsValid() || Properties.IsEmpty()) { return false; }
    auto Groups = Impl->GroupFlagProperties.ToNative();
    auto Object = MakeShared<FJsonObject>(); const TSharedPtr<FJsonObject>* Existing = nullptr;
    if (Groups->TryGetObjectField(Type, Existing)) { PostHogJson::Merge(*Object, **Existing); }
    PostHogJson::Merge(*Object, *Properties.ToNative()); Groups->SetObjectField(Type, Object);
    auto Candidate = FPostHogProperties::FromNative(Groups);
    if (!Candidate.IsValid()) { return false; }
    if (SameProperties(Candidate, Impl->GroupFlagProperties))
    { if (bReload) { Impl->ReloadFlagsLocked(); } return true; }
    auto Previous = Impl->GroupFlagProperties; Impl->GroupFlagProperties = MoveTemp(Candidate);
    if (!Impl->CanPersistLocked()) { Impl->GroupFlagProperties = MoveTemp(Previous); return false; }
    Impl->StaleFlagsLocked(); Impl->PersistLocked();
    if (bReload) { Impl->ReloadFlagsLocked(); }
    return true;
}
void FPostHogClient::ResetPersonPropertiesForFlags(bool bReload)
{
    FScopeLock Lock(&Impl->Mutex);
    if (!Impl->CanCaptureLocked() || Impl->PersonFlagProperties.IsEmpty()) { return; }
    Impl->PersonFlagProperties = {}; Impl->StaleFlagsLocked(); Impl->PersistLocked();
    if (bReload) { Impl->ReloadFlagsLocked(); }
}
void FPostHogClient::ResetGroupPropertiesForFlags(const FString& Type, bool bReload)
{
    FScopeLock Lock(&Impl->Mutex);
    if (!Impl->CanCaptureLocked() || Impl->GroupFlagProperties.IsEmpty()) { return; }
    auto Previous = Impl->GroupFlagProperties;
    if (Type.IsEmpty()) { Impl->GroupFlagProperties = {}; } else { Impl->GroupFlagProperties.Remove(Type); }
    if (SameProperties(Previous, Impl->GroupFlagProperties)) { return; }
    Impl->StaleFlagsLocked(); Impl->PersistLocked();
    if (bReload) { Impl->ReloadFlagsLocked(); }
}
FString FPostHogClient::GetSessionId()
{
    FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return FString(); }
    return Impl->SessionLocked(Impl->Config.Now(), false);
}
bool FPostHogClient::StartSession()
{
    FScopeLock Lock(&Impl->Mutex); if (!Impl->CanCaptureLocked()) { return false; }
    if (Impl->SessionId.IsEmpty()) { Impl->StartSessionLocked(Impl->Config.Now()); }
    return true;
}
void FPostHogClient::EndSession()
{
    { FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed) { return; } Impl->SessionId.Empty(); }
    FlushAsync();
}
bool FPostHogClient::SetSessionId(const FString& Id)
{
    FGuid Guid;
    if (!FGuid::ParseExact(Id, EGuidFormats::DigitsWithHyphens, Guid) || Id.Len() != 36 || Id[14] != TEXT('7') ||
        !(Id[19] == TEXT('8') || Id[19] == TEXT('9') || FChar::ToLower(Id[19]) == TEXT('a') || FChar::ToLower(Id[19]) == TEXT('b'))) { return false; }
    FScopeLock Lock(&Impl->Mutex); if (!Impl->CanCaptureLocked()) { return false; }
    Impl->SessionId = Id.ToLower(); Impl->SessionStarted = Impl->Config.Now(); Impl->LastActivity = Impl->SessionStarted; return true;
}
void FPostHogClient::StartApplicationLifecycle(bool bInForeground)
{
    TArray<FPostHogEvent> Events;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bClosed || Impl->bLifecycleStarted) { return; }
        Impl->bLifecycleStarted = true; Impl->bForeground = bInForeground;
        if (!Impl->Config.bCaptureApplicationLifecycleEvents) { return; }
        FString Version, Build;
        const auto Context = Impl->Config.Context.ToNative();
        Context->TryGetStringField(TEXT("$app_version"), Version); Context->TryGetStringField(TEXT("$app_build"), Build);
        if ((!Version.IsEmpty() || !Build.IsEmpty()) && (Version != Impl->LastAppVersion || Build != Impl->LastAppBuild))
        {
            const FString PreviousVersion = Impl->LastAppVersion, PreviousBuild = Impl->LastAppBuild;
            Impl->LastAppVersion = Version; Impl->LastAppBuild = Build;
            if (Impl->CanPersistLocked())
            {
                FPostHogProperties Properties;
                if (!PreviousVersion.IsEmpty()) { Properties.SetString(TEXT("previous_version"), PreviousVersion); }
                if (!PreviousBuild.IsEmpty()) { Properties.SetString(TEXT("previous_build"), PreviousBuild); }
                if (Impl->CanCaptureLocked())
                { Events.Add(Impl->SnapshotLocked(PreviousVersion.IsEmpty() && PreviousBuild.IsEmpty() ? TEXT("Application Installed") : TEXT("Application Updated"), Properties)); }
                Impl->PersistLocked();
            }
            else { Impl->LastAppVersion = PreviousVersion; Impl->LastAppBuild = PreviousBuild; }
        }
        if (bInForeground && Impl->CanCaptureLocked())
        { FPostHogProperties Properties; Properties.SetBoolean(TEXT("from_background"), false); Events.Add(Impl->SnapshotLocked(TEXT("Application Opened"), Properties)); }
    }
    for (auto& Event : Events) { Impl->Emit(MoveTemp(Event)); }
}
void FPostHogClient::OnBackground()
{
    TOptional<FPostHogEvent> Event;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bClosed || !Impl->bForeground) { return; }
        if (Impl->bLifecycleStarted && Impl->Config.bCaptureApplicationLifecycleEvents && Impl->CanCaptureLocked())
        { Event = Impl->SnapshotLocked(TEXT("Application Backgrounded"), {}); }
        Impl->bForeground = false;
        if (!Impl->SessionId.IsEmpty()) { Impl->LastActivity = Impl->Config.Now(); }
    }
    if (Event.IsSet()) { Impl->Emit(MoveTemp(Event.GetValue())); }
    FlushAsync();
}
void FPostHogClient::OnForeground()
{
    TOptional<FPostHogEvent> Event;
    {
        FScopeLock Lock(&Impl->Mutex); if (Impl->bClosed || Impl->bForeground) { return; }
        Impl->bForeground = true; Impl->SessionLocked(Impl->Config.Now(), true);
        if (Impl->bLifecycleStarted && Impl->Config.bCaptureApplicationLifecycleEvents && Impl->CanCaptureLocked())
        { FPostHogProperties Properties; Properties.SetBoolean(TEXT("from_background"), true); Event = Impl->SnapshotLocked(TEXT("Application Opened"), Properties); }
    }
    if (Event.IsSet()) { Impl->Emit(MoveTemp(Event.GetValue())); }
}
EPostHogCaptureResult FPostHogClient::SubmitReplaySnapshot(const FString& SessionId, int32 Width, int32 Height, const TArray64<uint8>& Jpeg, const FDateTime& CapturedAt)
{
    if (!NonBlank(SessionId) || Width <= 0 || Height <= 0 || Jpeg.IsEmpty() || Jpeg.Num() > PHU_MAX_REPLAY_INPUT) { return EPostHogCaptureResult::Invalid; }
    FString DistinctId;
    uint64 AdmissionGeneration = 0;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bClosed || Impl->Handle == 0) { return EPostHogCaptureResult::Closed; }
        if (!Impl->CanCaptureLocked() || !Impl->Config.Replay.bEnabled) { return EPostHogCaptureResult::Disabled; }
        DistinctId = Impl->DistinctId; AdmissionGeneration = Impl->ConsentGeneration;
    }
    const double Timestamp = static_cast<double>(PostHogJson::UnixMilliseconds(CapturedAt));
    auto Meta = MakeShared<FJsonObject>();
    Meta->SetNumberField(TEXT("width"), Width); Meta->SetNumberField(TEXT("height"), Height);
    Meta->SetStringField(TEXT("href"), FApp::GetProjectName());
    auto MetaEvent = MakeShared<FJsonObject>();
    MetaEvent->SetNumberField(TEXT("type"), 4); MetaEvent->SetNumberField(TEXT("timestamp"), Timestamp); MetaEvent->SetObjectField(TEXT("data"), Meta);
    auto Offset = MakeShared<FJsonObject>();
    Offset->SetNumberField(TEXT("top"), 0); Offset->SetNumberField(TEXT("left"), 0);
    auto Wireframe = MakeShared<FJsonObject>();
    Wireframe->SetNumberField(TEXT("id"), 1); Wireframe->SetNumberField(TEXT("x"), 0); Wireframe->SetNumberField(TEXT("y"), 0);
    Wireframe->SetNumberField(TEXT("width"), Width); Wireframe->SetNumberField(TEXT("height"), Height);
    Wireframe->SetStringField(TEXT("type"), TEXT("screenshot"));
    Wireframe->SetStringField(TEXT("base64"), TEXT("data:image/jpeg;base64,") + FBase64::Encode(Jpeg.GetData(), static_cast<uint32>(Jpeg.Num())));
    auto Full = MakeShared<FJsonObject>();
    Full->SetObjectField(TEXT("initialOffset"), Offset);
    TArray<TSharedPtr<FJsonValue>> Wireframes; Wireframes.Add(MakeShared<FJsonValueObject>(Wireframe));
    Full->SetArrayField(TEXT("wireframes"), Wireframes);
    auto FullEvent = MakeShared<FJsonObject>();
    FullEvent->SetNumberField(TEXT("type"), 2); FullEvent->SetNumberField(TEXT("timestamp"), Timestamp); FullEvent->SetObjectField(TEXT("data"), Full);
    auto Properties = MakeShared<FJsonObject>();
    Properties->SetStringField(TEXT("$snapshot_source"), TEXT("mobile"));
    Properties->SetStringField(TEXT("$session_id"), SessionId);
    Properties->SetStringField(TEXT("$window_id"), SessionId);
    Properties->SetStringField(TEXT("$lib"), PostHogJson::LibName);
    Properties->SetStringField(TEXT("$lib_version"), PostHogJson::LibVersion);
    TArray<TSharedPtr<FJsonValue>> SnapshotData;
    SnapshotData.Add(MakeShared<FJsonValueObject>(MetaEvent)); SnapshotData.Add(MakeShared<FJsonValueObject>(FullEvent));
    Properties->SetArrayField(TEXT("$snapshot_data"), SnapshotData);
    auto Envelope = MakeShared<FJsonObject>();
    Envelope->SetStringField(TEXT("event"), TEXT("$snapshot"));
    Envelope->SetStringField(TEXT("distinct_id"), DistinctId);
    Envelope->SetStringField(TEXT("uuid"), PostHogJson::NewUuidV7(CapturedAt));
    Envelope->SetStringField(TEXT("timestamp"), CapturedAt.ToIso8601());
    Envelope->SetObjectField(TEXT("properties"), Properties);
    // Frames exceed the analytics JSON bound, so this path is limited only by the native replay input size.
    const FTCHARToUTF8 Utf8(*PostHogJson::Encode(Envelope));
    if (static_cast<uint64>(Utf8.Length()) > PHU_MAX_REPLAY_INPUT) { return EPostHogCaptureResult::Invalid; }
    phu_handle NativeHandle = 0;
    {
        FScopeLock Lock(&Impl->Mutex);
        if (Impl->bClosed || Impl->Handle == 0) { return EPostHogCaptureResult::Closed; }
        if (Impl->bOptedOut || Impl->ConsentGeneration != AdmissionGeneration) { return EPostHogCaptureResult::Disabled; }
        NativeHandle = Impl->Handle;
    }
    // Unlike Emit, call unlocked: native re-parses a multi-hundred-KB frame, and a closed handle (never reused) returns PHU_CLOSED.
    return FromNative(phu_capture_replay(NativeHandle, View(Utf8)));
}
bool FPostHogClient::IsSessionReplayAllowed() const
{
    FScopeLock Lock(&Impl->Mutex);
    return Impl->CanCaptureLocked() && Impl->Config.Replay.bEnabled && Impl->FlagCache.SessionRecording.IsSet();
}
void FPostHogClient::ShutdownAll()
{
    // Replay workers call into clients, so they must be joined before clients close.
    FPostHogReplayRecorder::StopAll();
    TArray<TSharedPtr<FImpl, ESPMode::ThreadSafe>> Live;
    { FScopeLock Lock(&ClientsMutex); bStopping = true;
        for (const auto& Weak : Clients) { if (auto Client = Weak.Pin()) { Live.Add(Client); } } Clients.Empty(); }
    for (const auto& Client : Live) { Client->Close(); }
    PostHogTasks::Drain();
}
