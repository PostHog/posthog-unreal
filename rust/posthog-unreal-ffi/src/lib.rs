//! ABI v3: marshaling and lifetimes over the public stateful SDK.
mod abi;
mod flags;
mod handles;
mod hooks;

use std::collections::HashMap;
use std::fmt::Write as _;
use std::sync::Arc;
use std::time::Duration;

use chrono::{DateTime, Utc};
use posthog_rs::stateful::{
    self, CaptureOutcome, DeliveryOutcome, DrainReport, PersistenceOutcome, SubmitOutcome,
};
use serde::Deserialize;
use serde_json::{json, Value};
use sha1::{Digest, Sha1};
use uuid::Uuid;

use abi::{clear, contain, output, parse, Result, ABI_VERSION};
pub use abi::{phu_abi_version, phu_buffer_copy, phu_buffer_free, Buffer, Bytes, Status};
pub use hooks::Hooks;

#[derive(Deserialize)]
#[serde(default, deny_unknown_fields)]
struct Options {
    project_token: String,
    host: String,
    storage_root: Option<String>,
    instance_name: String,
    disabled: bool,
    opt_out: bool,
    person_profiles: Profiles,
    session_mode: Sessions,
    persist_sessions: bool,
    preload_feature_flags: bool,
    send_feature_flag_event: bool,
    platform_context: HashMap<String, Value>,
    flush_at: usize,
    flush_interval_ms: u64,
    max_queue_size: usize,
    max_batch_size: usize,
    request_timeout_seconds: u64,
    shutdown_timeout_ms: u64,
    jpeg_quality: u8,
    max_frames_in_flight: usize,
}
#[derive(Default, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Profiles {
    Never,
    #[default]
    IdentifiedOnly,
    Always,
}
#[derive(Default, Deserialize)]
#[serde(rename_all = "snake_case")]
enum Sessions {
    #[default]
    Automatic,
    Manual,
}
impl Default for Options {
    fn default() -> Self {
        Self {
            project_token: String::new(),
            host: posthog_rs::DEFAULT_HOST.into(),
            storage_root: None,
            instance_name: "default".into(),
            disabled: false,
            opt_out: false,
            person_profiles: Profiles::default(),
            session_mode: Sessions::default(),
            persist_sessions: false,
            preload_feature_flags: true,
            send_feature_flag_event: true,
            platform_context: HashMap::new(),
            flush_at: 20,
            flush_interval_ms: 30000,
            max_queue_size: 1000,
            max_batch_size: 50,
            request_timeout_seconds: 10,
            shutdown_timeout_ms: 3000,
            jpeg_quality: 65,
            max_frames_in_flight: 2,
        }
    }
}
impl Options {
    fn config(self) -> stateful::Config {
        let mut config = stateful::Config::new(self.project_token);
        config.host = self.host;
        config.persistence = self
            .storage_root
            .map_or(stateful::Persistence::Memory, |root| {
                stateful::Persistence::Directory(root.into())
            });
        config.instance_name = self.instance_name;
        config.disabled = self.disabled;
        config.opt_out = self.opt_out;
        config.person_profiles = match self.person_profiles {
            Profiles::Never => stateful::PersonProfiles::Never,
            Profiles::IdentifiedOnly => stateful::PersonProfiles::IdentifiedOnly,
            Profiles::Always => stateful::PersonProfiles::Always,
        };
        config.session_mode = match self.session_mode {
            Sessions::Automatic => stateful::SessionMode::Automatic,
            Sessions::Manual => stateful::SessionMode::Manual,
        };
        config.session_persistence = if self.persist_sessions {
            stateful::SessionPersistence::Persistent
        } else {
            stateful::SessionPersistence::Memory
        };
        config.preload_feature_flags = self.preload_feature_flags;
        config.send_feature_flag_event = self.send_feature_flag_event;
        config.platform_context = self.platform_context;
        config.flush_at = self.flush_at;
        config.flush_interval = Duration::from_millis(self.flush_interval_ms);
        config.max_queue_size = self.max_queue_size;
        config.max_batch_size = self.max_batch_size;
        config.request_timeout_seconds = self.request_timeout_seconds;
        config.shutdown_timeout = Duration::from_millis(self.shutdown_timeout_ms);
        config.session_replay.jpeg_quality = self.jpeg_quality;
        config.session_replay.max_frames_in_flight = self.max_frames_in_flight;
        config.sdk_name = "posthog-unreal".into();
        config.sdk_version = env!("CARGO_PKG_VERSION").into();
        config
    }
}

/// # Safety
/// Input readable, output writable/disjoint. Pure coordination lookup: no client or I/O.
#[no_mangle]
pub unsafe extern "C" fn phu_coordination_key(input: Bytes, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let options: Options = unsafe { parse(input)? };
        // Match Config's input normalization; endpoint aliases remain SDK-owned.
        let host = options.host.trim();
        let host = posthog_rs::EndpointManager::determine_server_host(if host.is_empty() {
            posthog_rs::DEFAULT_HOST
        } else {
            host
        });
        // An unambiguous opaque host/token/instance key, not a storage namespace/path.
        let fields = [
            host.as_str(),
            options.project_token.trim(),
            options.instance_name.as_str(),
        ];
        let mut key = String::with_capacity(40);
        for byte in Sha1::digest(abi::encode(&fields, abi::MAX_INPUT)?).iter() {
            write!(&mut key, "{byte:02x}").expect("writing to a String cannot fail");
        }
        unsafe {
            output(out, &key)?;
        }
        Ok(Status::Ok)
    })
}

/// # Safety
/// All bytes are readable, outputs writable/disjoint, and hooks follow the header lifetime contract.
#[no_mangle]
pub unsafe extern "C" fn phu_create(
    version: u32,
    options: Bytes,
    hooks: Hooks,
    out: *mut u64,
) -> Status {
    contain(|| {
        if out.is_null() {
            return Err(Status::InvalidInput);
        }
        unsafe {
            *out = 0;
        }
        if version != ABI_VERSION {
            return Err(Status::UnsupportedAbi);
        }
        let options: Options = unsafe { parse(options)? };
        let mut config = options.config();
        let timeout = config.shutdown_timeout.min(Duration::from_secs(86400));
        let scope = Arc::new(hooks::CallbackScope);
        hooks::install(&mut config, hooks, scope.clone());
        let mailbox = Arc::new(flags::Mailbox::default());
        let entry = Arc::new(handles::Entry::new(
            stateful::Client::init_blocking(config),
            mailbox,
            scope,
            timeout,
        ));
        let id = {
            let mut registry = handles::registry()
                .lock()
                .unwrap_or_else(|p| p.into_inner());
            let id = registry.next()?;
            registry.clients.insert(id, entry);
            id
        };
        unsafe {
            *out = id;
        }
        Ok(Status::Ok)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Capture {
    event: String,
    #[serde(default)]
    fallback_properties: HashMap<String, Value>,
    #[serde(default)]
    properties: HashMap<String, Value>,
    #[serde(default)]
    options: HashMap<String, Value>,
    #[serde(default)]
    groups: HashMap<String, String>,
    timestamp: Option<DateTime<Utc>>,
    uuid: Option<Uuid>,
}
fn captured(result: CaptureOutcome, out: *mut Buffer) -> Result<Status> {
    use CaptureOutcome::*;
    Ok(match result {
        Accepted { uuid } => {
            unsafe {
                output(out, &json!({"uuid":uuid}))?;
            }
            Status::Accepted
        }
        Disabled => Status::Disabled,
        OptedOut => Status::OptedOut,
        Closed => Status::Closed,
        Invalid => Status::InvalidInput,
        Dropped => Status::Dropped,
        Reentrant => Status::Reentrant,
        Full => Status::Full,
        _ => Status::InternalError,
    })
}
/// # Safety
/// Input readable and output writable/disjoint. Returned receipt is owned by the caller.
#[no_mangle]
pub unsafe extern "C" fn phu_capture(handle: u64, input: Bytes, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let call = handles::acquire(handle)?;
        let input: Capture = unsafe { parse(input)? };
        let mut capture = stateful::Capture::new(input.event);
        capture.properties = input.properties;
        capture.fallback_properties = input.fallback_properties;
        capture.options = input.options;
        capture.groups = input.groups;
        capture.timestamp = input.timestamp;
        capture.uuid = input.uuid;
        captured(call.0.client.capture(capture), out)
    })
}
#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Screen {
    name: String,
    #[serde(default)]
    properties: HashMap<String, Value>,
}
/// # Safety
/// Same pointer/ownership contract as phu_capture.
#[no_mangle]
pub unsafe extern "C" fn phu_screen(handle: u64, input: Bytes, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let call = handles::acquire(handle)?;
        let input: Screen = unsafe { parse(input)? };
        captured(call.0.client.screen(input.name, input.properties), out)
    })
}

#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
enum Command {
    Identify {
        distinct_id: String,
        #[serde(default)]
        set: HashMap<String, Value>,
        #[serde(default)]
        set_once: HashMap<String, Value>,
    },
    Reset {},
    Alias {
        alias: String,
    },
    Register {
        properties: HashMap<String, Value>,
    },
    Unregister {
        key: String,
    },
    SetPersonProperties {
        set: HashMap<String, Value>,
        #[serde(default)]
        set_once: HashMap<String, Value>,
    },
    Group {
        group_type: String,
        group_key: String,
        properties: Option<HashMap<String, Value>>,
    },
    GroupIdentify {
        group_type: String,
        group_key: String,
        properties: HashMap<String, Value>,
    },
    RemoveGroup {
        group_type: String,
    },
    OptIn {},
    OptOut {},
    StartSession {},
    EndSession {},
    ResetSession {},
    SessionActivity {},
    SetAppInBackground {
        background: bool,
    },
    ReloadFeatureFlags {},
    SetPersonPropertiesForFlags {
        properties: HashMap<String, Value>,
        reload: Option<bool>,
    },
    ResetPersonPropertiesForFlags {
        reload: Option<bool>,
    },
    SetGroupPropertiesForFlags {
        group_type: String,
        properties: HashMap<String, Value>,
        reload: Option<bool>,
    },
    ResetGroupPropertiesForFlags {
        group_type: Option<String>,
        reload: Option<bool>,
    },
    ReplayStart {},
    ReplayStop {},
}
/// # Safety
/// Input readable for its length. OK means invocation, not mutation/admission/durability confirmation.
#[no_mangle]
pub unsafe extern "C" fn phu_command(handle: u64, input: Bytes) -> Status {
    contain(|| {
        let call = handles::acquire(handle)?;
        let command: Command = unsafe { parse(input)? };
        let client = &call.0.client;
        use Command::*;
        match command {
            Identify {
                distinct_id,
                set,
                set_once,
            } => {
                let mut id = stateful::Identify::new(distinct_id);
                id.set = set;
                id.set_once = set_once;
                client.identify(id);
            }
            Reset {} => client.reset(),
            Alias { alias } => client.alias(alias),
            Register { properties } => client.register(properties),
            Unregister { key } => client.unregister(&key),
            SetPersonProperties { set, set_once } => client.set_person_properties(set, set_once),
            Group {
                group_type,
                group_key,
                properties,
            } => client.group(group_type, group_key, properties),
            GroupIdentify {
                group_type,
                group_key,
                properties,
            } => client.group_identify(group_type, group_key, properties),
            RemoveGroup { group_type } => client.remove_group(&group_type),
            OptIn {} => client.opt_in(),
            OptOut {} => client.opt_out(),
            StartSession {} => client.start_session(),
            EndSession {} => client.end_session(),
            ResetSession {} => client.reset_session(),
            SessionActivity {} => client.session_activity(),
            SetAppInBackground { background } => client.set_app_in_background(background),
            ReloadFeatureFlags {} => client.reload_feature_flags(),
            SetPersonPropertiesForFlags { properties, reload } => {
                client.set_person_properties_for_flags(properties, reload)
            }
            ResetPersonPropertiesForFlags { reload } => {
                client.reset_person_properties_for_flags(reload)
            }
            SetGroupPropertiesForFlags {
                group_type,
                properties,
                reload,
            } => client.set_group_properties_for_flags(group_type, properties, reload),
            ResetGroupPropertiesForFlags { group_type, reload } => {
                client.reset_group_properties_for_flags(group_type.as_deref(), reload)
            }
            ReplayStart {} => client.session_replay().start(),
            ReplayStop {} => client.session_replay().stop(),
        }
        Ok(Status::Ok)
    })
}

#[derive(Deserialize)]
#[serde(tag = "op", rename_all = "snake_case", deny_unknown_fields)]
enum Read {
    State {},
    SessionId {},
    PeekSessionId {},
    FeatureFlag {
        key: String,
        send_event: Option<bool>,
    },
    FeatureFlags {},
    ReplayActive {},
}
/// # Safety
/// Input readable and output writable/disjoint. JSON null denotes an unavailable result.
#[no_mangle]
pub unsafe extern "C" fn phu_read(handle: u64, input: Bytes, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let call = handles::acquire(handle)?;
        let query: Read = unsafe { parse(input)? };
        let client = &call.0.client;
        let value = match query {
            Read::State {} => {
                json!({"disabled":client.is_disabled(),"identified":client.is_identified(),
                "opt_out":client.is_opt_out(),"anonymous_id":client.get_anonymous_id(),"distinct_id":client.get_distinct_id()})
            }
            Read::SessionId {} => json!(client.session_id()),
            Read::PeekSessionId {} => json!(client.peek_session_id()),
            Read::FeatureFlag { key, send_event } => {
                let lookup = client.feature_flag(&key);
                let lookup = if let Some(send) = send_event {
                    lookup.send_event(send)
                } else {
                    lookup
                };
                unsafe {
                    output(out, &lookup.get().as_ref().map(flags::flag))?;
                }
                return Ok(Status::Ok);
            }
            Read::FeatureFlags {} => {
                unsafe {
                    output(
                        out,
                        &client.get_feature_flags().as_ref().map(flags::FlagMap),
                    )?;
                }
                return Ok(Status::Ok);
            }
            Read::ReplayActive {} => json!(client.session_replay().is_active()),
        };
        unsafe {
            output(out, &value)?;
        }
        Ok(Status::Ok)
    })
}
/// # Safety
/// Output writable. Takes the latest coalesced notification; Inactive means no unread update.
#[no_mangle]
pub unsafe extern "C" fn phu_poll_flags(handle: u64, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let call = handles::acquire(handle)?;
        match call.0.mailbox.take() {
            Some(bytes) => {
                unsafe {
                    *out = Buffer::new(bytes?);
                }
                Ok(Status::Ok)
            }
            None => Ok(Status::Inactive),
        }
    })
}
fn drain(report: DrainReport) -> Value {
    use DeliveryOutcome as D;
    use PersistenceOutcome as P;
    let state = match report.state {
        P::Persisted => "persisted",
        P::MemoryOnly => "memory_only",
        P::Unavailable => "unavailable",
        P::WriteFailed => "write_failed",
        P::TimedOut => "timed_out",
        P::Deferred => "deferred",
        P::Disabled => "disabled",
        P::NotRequested => "not_requested",
        _ => "unknown",
    };
    let delivery = match report.delivery {
        D::Attempted => "attempted",
        D::Stopped => "stopped",
        D::Unavailable => "unavailable",
        D::Closed => "closed",
        D::Deferred => "deferred",
        D::Disabled => "disabled",
        _ => "unknown",
    };
    json!({"state":state,"delivery":delivery})
}
/// # Safety
/// Output writable. Blocking, except SDK callback-origin calls return a Deferred report.
#[no_mangle]
pub unsafe extern "C" fn phu_flush(handle: u64, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let call = handles::acquire(handle)?;
        unsafe {
            output(out, &drain(call.0.client.flush_blocking()))?;
        }
        Ok(Status::Ok)
    })
}
#[no_mangle]
pub extern "C" fn phu_begin_shutdown(handle: u64) -> Status {
    contain(|| {
        handles::begin(handle)?;
        Ok(Status::Ok)
    })
}
/// # Safety
/// Output writable. Deferred means callback-origin: finish from outside the callback before unload.
#[no_mangle]
pub unsafe extern "C" fn phu_close(handle: u64, out: *mut Buffer) -> Status {
    contain(|| {
        unsafe {
            clear(out)?;
        }
        let entry = handles::begin(handle)?;
        let report = entry.finish()?;
        // Core joins and admitted ABI calls have finished. Release the registry's ownership.
        let removed = handles::registry()
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .clients
            .remove(&handle);
        drop(removed);
        unsafe {
            output(out, &drain(report))?;
        }
        Ok(Status::Ok)
    })
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct Layout {
    width: u32,
    height: u32,
    row_stride_bytes: usize,
}
/// # Safety
/// Input readable, out writable. Layout is composited, downscaled, masked, top-down BGRA8888.
#[no_mangle]
pub unsafe extern "C" fn phu_begin_frame(handle: u64, layout: Bytes, out: *mut u64) -> Status {
    contain(|| {
        if out.is_null() {
            return Err(Status::InvalidInput);
        }
        unsafe {
            *out = 0;
        }
        let call = handles::acquire(handle)?;
        let layout: Layout = unsafe { parse(layout)? };
        let layout = stateful::FrameLayout {
            width: layout.width,
            height: layout.height,
            row_stride_bytes: layout.row_stride_bytes,
            format: stateful::PixelFormat::Bgra8888,
        };
        let frame = call
            .0
            .client
            .session_replay()
            .begin_frame(layout)
            .ok_or(Status::Inactive)?;
        let id = call.0.insert_frame(handle, frame)?;
        unsafe {
            *out = id;
        }
        Ok(Status::Ok)
    })
}
/// # Safety
/// Pixels readable for their exact acquired length; token is consumed on every submission attempt.
#[no_mangle]
pub unsafe extern "C" fn phu_submit_frame(handle: u64, frame: u64, pixels: Bytes) -> Status {
    contain(|| {
        let mut frame = handles::take_frame(frame)?;
        let call = handles::acquire(handle)?;
        if frame.owner != handle {
            return Err(Status::WrongRecorder);
        }
        let target = frame.frame.pixels_mut();
        if pixels.len != target.len() {
            return Err(Status::InvalidInput);
        }
        target.copy_from_slice(unsafe { pixels.slice(target.len())? });
        Ok(
            match call.0.client.session_replay().submit_frame(frame.frame) {
                SubmitOutcome::Accepted => Status::Accepted,
                SubmitOutcome::WrongRecorder => Status::WrongRecorder,
                SubmitOutcome::Reentrant => Status::Reentrant,
                SubmitOutcome::Stale => Status::Stale,
                SubmitOutcome::Closed => Status::Closed,
                _ => Status::InternalError,
            },
        )
    })
}
#[no_mangle]
pub extern "C" fn phu_abandon_frame(frame: u64) -> Status {
    contain(|| {
        drop(handles::take_frame(frame)?);
        Ok(Status::Ok)
    })
}

#[cfg(test)]
mod tests;
