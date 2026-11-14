//! The sole foreign callback: synchronous event filtering, with explicit host lifetime.
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::Arc;

use chrono::NaiveDateTime;
use posthog_rs::Event;
use serde::Deserialize;
use serde_json::Value;
use uuid::Uuid;

use crate::abi::{encode, Buffer, Bytes, Status, MAX_HOOK};

pub type BeforeSend = unsafe extern "C" fn(*mut c_void, Bytes, *mut Buffer) -> i32;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct Hooks {
    pub before_send: Option<BeforeSend>,
    pub user_data: *mut c_void,
}

thread_local! { static CALLBACKS: RefCell<Vec<usize>> = const { RefCell::new(Vec::new()) }; }
pub struct CallbackScope;
impl CallbackScope {
    pub fn active(&self) -> bool {
        CALLBACKS.with(|keys| keys.borrow().contains(&(self as *const Self as usize)))
    }
}
struct Guard;
impl Drop for Guard {
    fn drop(&mut self) {
        CALLBACKS.with(|keys| {
            keys.borrow_mut().pop();
        });
    }
}

#[derive(Deserialize)]
#[serde(deny_unknown_fields)]
struct HookEvent {
    event: String,
    distinct_id: String,
    properties: HashMap<String, Value>,
    #[serde(default)]
    options: HashMap<String, Value>,
    groups: HashMap<String, String>,
    timestamp: NaiveDateTime,
    uuid: Uuid,
}
impl HookEvent {
    fn into_event(self) -> Option<Event> {
        if self.event.trim().is_empty() || self.distinct_id.trim().is_empty() {
            return None;
        }
        let mut event = Event::new(self.event, self.distinct_id);
        for (key, value) in self.properties {
            event.insert_prop(key, value).ok()?;
        }
        for (key, value) in self.options {
            event.insert_option(key, value).ok()?;
        }
        for (key, value) in self.groups {
            event.add_group(&key, &value);
        }
        event.set_timestamp(self.timestamp.and_utc()).ok()?;
        event.set_uuid(self.uuid);
        Some(event)
    }
}

pub fn install(config: &mut posthog_rs::stateful::Config, hooks: Hooks, scope: Arc<CallbackScope>) {
    let Some(callback) = hooks.before_send else {
        return;
    };
    // The host promises this opaque address is thread-safe and remains valid until close joins.
    // Capture the address as an integer so Rust never dereferences a foreign context pointer.
    let user_data = hooks.user_data as usize;
    config.before_send(move |event| {
        let input = encode(&event, MAX_HOOK).ok()?;
        CALLBACKS.with(|keys| keys.borrow_mut().push(Arc::as_ptr(&scope) as usize));
        let _guard = Guard;
        let mut out = Buffer::empty();
        let status = unsafe {
            callback(
                user_data as *mut c_void,
                Bytes::from_slice(&input),
                &mut out,
            )
        };
        // Ownership transfers on every callback return, including failure/drop.
        let result = if status == Status::Ok as i32 {
            unsafe {
                Bytes {
                    data: out.data,
                    len: out.len,
                }
                .slice(MAX_HOOK)
            }
            .ok()
            .and_then(|bytes| serde_json::from_slice::<HookEvent>(bytes).ok())
            .and_then(HookEvent::into_event)
        } else {
            None
        };
        unsafe { out.free() };
        result
    });
}
