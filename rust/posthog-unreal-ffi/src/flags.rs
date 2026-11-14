//! A coalescing notification mailbox, not a flag cache. Values are always read from the SDK.
use std::collections::HashMap;
use std::sync::Mutex;

use posthog_rs::stateful::{
    FeatureFlagContext, FeatureFlagLoadError, FeatureFlagResult, FeatureFlagsUpdate,
};
use serde::ser::SerializeMap;
use serde::{Serialize, Serializer};
use serde_json::{json, Value};

use crate::abi::{encode, Result, MAX_OUTPUT};

#[derive(Serialize)]
struct Context<'a> {
    distinct_id: &'a str,
    anonymous_id: &'a str,
    groups: &'a HashMap<String, String>,
    person_properties: &'a HashMap<String, Value>,
    group_properties: &'a HashMap<String, HashMap<String, Value>>,
    disable_geoip: bool,
}
fn context(value: &FeatureFlagContext) -> Context<'_> {
    Context {
        distinct_id: &value.distinct_id,
        anonymous_id: &value.anonymous_id,
        groups: &value.groups,
        person_properties: &value.person_properties,
        group_properties: &value.group_properties,
        disable_geoip: value.disable_geoip,
    }
}
#[derive(Serialize)]
pub struct Flag<'a> {
    key: &'a str,
    enabled: bool,
    variant: &'a Option<String>,
    has_payload: bool,
    payload: &'a Option<Value>,
    context: Context<'a>,
    evaluation_revision: u64,
    request_id: &'a Option<String>,
    evaluated_at: Option<i64>,
    partial: bool,
    id: Option<u64>,
    version: Option<u32>,
    reason: &'a Option<String>,
}
pub fn flag(value: &FeatureFlagResult) -> Flag<'_> {
    Flag {
        key: &value.key,
        enabled: value.enabled,
        variant: &value.variant,
        has_payload: value.payload.is_some(),
        payload: &value.payload,
        context: context(&value.context),
        evaluation_revision: value.evaluation_revision,
        request_id: &value.request_id,
        evaluated_at: value.evaluated_at,
        partial: value.partial,
        id: value.id,
        version: value.version,
        reason: &value.reason,
    }
}
pub struct FlagMap<'a>(pub &'a HashMap<String, FeatureFlagResult>);
impl Serialize for FlagMap<'_> {
    fn serialize<S: Serializer>(&self, serializer: S) -> std::result::Result<S::Ok, S::Error> {
        let mut map = serializer.serialize_map(Some(self.0.len()))?;
        for (key, value) in self.0 {
            map.serialize_entry(key, &flag(value))?;
        }
        map.end()
    }
}
fn error(value: FeatureFlagLoadError) -> Value {
    use FeatureFlagLoadError::*;
    match value {
        RequestTooLarge => json!({"kind":"request_too_large"}),
        Network => json!({"kind":"network"}),
        Http(status) => json!({"kind":"http","status":status}),
        ResponseTooLarge => json!({"kind":"response_too_large"}),
        InvalidResponse => json!({"kind":"invalid_response"}),
        TooManyFlags => json!({"kind":"too_many_flags"}),
        QuotaLimited => json!({"kind":"quota_limited"}),
        CacheTooLarge => json!({"kind":"cache_too_large"}),
        _ => json!({"kind":"unknown"}),
    }
}
#[derive(Serialize)]
struct Update<'a> {
    flags: Option<FlagMap<'a>>,
    context: Context<'a>,
    errors_loading: bool,
    error: Option<Value>,
    partial: bool,
    invalid_payloads: bool,
}
#[derive(Default)]
pub struct Mailbox(Mutex<Inbox>);
#[derive(Default)]
struct Inbox {
    closed: bool,
    latest: Option<Result<Vec<u8>>>,
}
impl Mailbox {
    pub fn push(&self, update: &FeatureFlagsUpdate) {
        let bytes = encode(
            &Update {
                flags: update.flags.as_ref().map(FlagMap),
                context: context(&update.context),
                errors_loading: update.errors_loading,
                error: update.error.map(error),
                partial: update.partial,
                invalid_payloads: update.invalid_payloads,
            },
            MAX_OUTPUT,
        );
        let mut inbox = self.0.lock().unwrap_or_else(|p| p.into_inner());
        if !inbox.closed {
            inbox.latest = Some(bytes);
        }
    }
    pub fn take(&self) -> Option<Result<Vec<u8>>> {
        self.0
            .lock()
            .unwrap_or_else(|p| p.into_inner())
            .latest
            .take()
    }
    pub fn close(&self) {
        let mut inbox = self.0.lock().unwrap_or_else(|p| p.into_inner());
        inbox.closed = true;
        inbox.latest = None;
    }
}
