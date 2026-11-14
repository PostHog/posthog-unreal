#ifndef POSTHOG_UNREAL_FFI_H
#define POSTHOG_UNREAL_FFI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define PHU_ABI_VERSION 3u
#define PHU_MAX_INPUT (1024u * 1024u)
#define PHU_MAX_OUTPUT (4u * 1024u * 1024u)
#define PHU_MAX_HOOK (32u * 1024u * 1024u)
typedef uint64_t phu_handle;
typedef uint64_t phu_frame;
typedef int32_t phu_status;
enum {
    PHU_OK = 0, PHU_ACCEPTED = 1, PHU_INVALID_INPUT = 2, PHU_CLOSED = 3,
    PHU_INTERNAL_ERROR = 5, PHU_UNSUPPORTED_ABI = 6, PHU_DISABLED = 7,
    PHU_FULL = 8, PHU_OUTPUT_TOO_LARGE = 9, PHU_OPTED_OUT = 10,
    PHU_DROPPED = 11, PHU_REENTRANT = 12, PHU_DEFERRED = 13,
    PHU_INACTIVE = 14, PHU_WRONG_RECORDER = 15, PHU_STALE = 16
};
typedef struct phu_bytes { const uint8_t* data; size_t len; } phu_bytes;
typedef struct phu_buffer { uint8_t* data; size_t len; } phu_buffer;

/* All JSON is UTF-8, not NUL-terminated. Inputs are borrowed only during calls.
 * Outputs must be writable/disjoint from inputs and are zeroed before fallible work.
 * Release successful buffers exactly once with phu_buffer_free, including JSON null.
 * Invalid non-NULL pointers cannot be validated. No exceptions may cross this ABI.
 * Normal JSON input/output limits are PHU_MAX_INPUT/PHU_MAX_OUTPUT.
 */
uint32_t phu_abi_version(void);
phu_status phu_buffer_copy(phu_bytes input, phu_buffer* out_buffer);
void phu_buffer_free(phu_buffer buffer);

/* Hook event JSON: {event, distinct_id, properties, options?, groups, timestamp, uuid}.
 * timestamp is a UTC ISO datetime WITHOUT timezone suffix (SDK Event serialization).
 * Hooks see the assembled SDK event, including options. Return PHU_OK and a complete
 * transformed event, or any other status to drop. Invalid/missing/oversized output drops
 * the event; the original is never sent as fallback. Preserve fields not edited by the
 * host. Required screenshot attribution cannot be changed (SDK validates it).
 * Input is borrowed; do not retain it. Output must be a fresh unchanged allocation
 * from phu_buffer_copy (limit PHU_MAX_HOOK), or {NULL,0}; ownership transfers to Rust
 * on EVERY return, including failures. Never free a transferred output in the host.
 * Callback and user_data must be thread-safe and remain valid through completed close.
 * Analytics runs on the capturing thread; replay runs on the encoder worker. No UI
 * thread dispatch is provided. Same-client recursive capture returns REENTRANT.
 * Do not wait for another capture from a hook. Catch foreign exceptions in the host
 * and return a drop status. Rust panic containment cannot catch a foreign exception.
 */
typedef phu_status (*phu_before_send)(void* user_data, phu_bytes event, phu_buffer* out_event);
typedef struct phu_hooks { phu_before_send before_send; void* user_data; } phu_hooks;

/* Pure pre-initialization ownership key; no client, filesystem or network access.
 * Input: {project_token, host?, instance_name?}, using the create defaults.
 * Trims host/token, defaults an empty host, and uses the SDK endpoint helper for
 * aliases/trailing slashes. OK returns an opaque JSON string for host/token/instance
 * equality only. Combine with the engine-selected storage root for coordination.
 * This is NOT a Rust storage namespace/path or a persistence guarantee.
 */
phu_status phu_coordination_key(phu_bytes input, phu_buffer* out_key);

/* Options (all but project_token optional):
 * {project_token, host (https://us.i.posthog.com), storage_root?, instance_name (default),
 * disabled (false), opt_out (false), person_profiles (never|identified_only|always),
 * session_mode (automatic|manual), persist_sessions (false), preload_feature_flags (true),
 * send_feature_flag_event (true), platform_context ({}), flush_at (20),
 * flush_interval_ms (30000), max_queue_size (1000), max_batch_size (50),
 * request_timeout_seconds (10), shutdown_timeout_ms (3000), jpeg_quality (65),
 * max_frames_in_flight (2)}.
 * Absent/null storage_root selects MEMORY, otherwise Rust namespaces the supplied root
 * by normalized host/project/instance and owns state/events. No legacy state import.
 * Core config/startup failures produce a disabled client; storage failures permit
 * memory fallback. Inspect phu_read(state) and drain outcomes, not the create status.
 * Creates native workers and performs local I/O: call off the game thread.
 * Returns a nonzero handle on OK. Handles are never reused. Hooks may be {NULL,NULL}.
 */
phu_status phu_create(uint32_t abi_version, phu_bytes options, phu_hooks hooks, phu_handle* out_handle);

/* Input: {event, properties?, fallback_properties?, options?, groups?, timestamp? (RFC3339), uuid?}.
 * fallback_properties fills only absent assembled property keys before before-send; it is not registered state.
 * Identity/session/enrichment come from Rust. No disk/network on caller; hooks run inline.
 * ACCEPTED returns {uuid} in out_receipt and means bounded admission, not persistence or
 * ingestion. Other capture outcomes have no buffer. Screen input: {name, properties?}.
 */
phu_status phu_capture(phu_handle handle, phu_bytes input, phu_buffer* out_receipt);
phu_status phu_screen(phu_handle handle, phu_bytes input, phu_buffer* out_receipt);

/* Commands are tagged JSON: {op, ...fields}. OK means the public SDK method was
 * invoked, NOT confirmation it changed state or admitted an event. Core methods
 * may no-op (disabled, invalid semantics, duplicate identity, capacity, etc.).
 * Supported op / additional fields:
 * identify: distinct_id, set? {}, set_once? {}
 * reset; alias: alias; register: properties; unregister: key
 * set_person_properties: set, set_once? {}
 * group: group_type, group_key, properties?
 * group_identify: group_type, group_key, properties; remove_group: group_type
 * opt_in; opt_out; start_session; end_session; reset_session; session_activity
 * set_app_in_background: background
 * reload_feature_flags
 * set_person_properties_for_flags: properties, reload?
 * reset_person_properties_for_flags: reload?
 * set_group_properties_for_flags: group_type, properties, reload?
 * reset_group_properties_for_flags: group_type?, reload? (absent type clears all)
 * replay_start; replay_stop
 * reload absent/null uses core default (true). Mutations/persistence are Rust-owned.
 * Consent changes affect new collection immediately; previously permitted work may
 * still persist/deliver. Reset restores the configured default consent and clears sessions.
 */
phu_status phu_command(phu_handle handle, phu_bytes command);

/* Read {op,...fields}:
 * state -> {disabled,identified,opt_out,anonymous_id,distinct_id} (individual SDK reads)
 * session_id -> UUID string|null (may maintain session); peek_session_id -> string|null
 *   (passive, may return expired state, per SDK contract)
 * feature_flag: key, send_event? -> rich result|null (omission uses SDK reporting default)
 * feature_flags -> map of rich results|null, including known false, no exposures
 * replay_active -> bool (passive)
 * Rich result: {key,enabled,variant,has_payload,payload,context,evaluation_revision,
 * request_id,evaluated_at,partial,id,version,reason}. Nullable fields stay null;
 * has_payload distinguishes absent payload from present JSON null. Context contains
 * distinct_id,anonymous_id,groups,person_properties,group_properties,disable_geoip.
 * Feature reads remain available while opted out; exposure still obeys consent.
 */
phu_status phu_read(phu_handle handle, phu_bytes query, phu_buffer* out_buffer);

/* One-slot, last-received notification mailbox; unread notifications coalesce.
 * Poll on the game thread. INACTIVE means empty. OK consumes an owned notification:
 * {flags (ENABLED results only, or null),context,errors_loading,error,partial,invalid_payloads}.
 * error is null or {kind,status?}. OUTPUT_TOO_LARGE consumes an oversized notification.
 * This observes bootstrap/restored/automatic/manual loads, not request-correlated futures.
 * Data is the SDK's immutable callback snapshot, not proof of current targeting by
 * game-thread delivery time. Use core getters for current cached values. Close cancels
 * unread updates and explicitly unsubscribes; native worker joins finish running callbacks.
 */
phu_status phu_poll_flags(phu_handle handle, phu_buffer* out_update);

/* Blocking local progress, never an ingestion receipt. JSON {state,delivery}:
 * state: persisted|memory_only|unavailable|write_failed|timed_out|deferred|disabled|not_requested
 * delivery: attempted|stopped|unavailable|closed|deferred|disabled
 * Future core variants map to "unknown". Flush includes earlier submitted encoding and
 * eligible replay delivery, not host-held frames or retry cooldown bypass.
 * Hook-origin flush requests work and reports deferred rather than waiting.
 */
phu_status phu_flush(phu_handle handle, phu_buffer* out_report);

/* Start closes bridge admission immediately, selects one shared best-effort budget,
 * abandons host-held frames, unsubscribes listeners and schedules tracked core shutdown.
 * Worker scheduling delay is deducted before entering core shutdown. Repeated start
 * does not extend the budget. No detached work; finish must join before module unload.
 * Existing I/O, callbacks and joins may exceed the budget. Start may return INTERNAL_ERROR
 * if thread creation fails; finish still performs cleanup synchronously.
 * Close starts shutdown if needed, waits admitted ABI calls, joins core work and releases
 * the handle. Concurrent finishers which already acquired the handle share completion.
 * Invalid/already-released handles return CLOSED (not proof another finisher completed).
 * From this client's hook, close starts shutdown and returns DEFERRED with no buffer:
 * return from the hook, then finish from another thread. DEFERRED is NOT unload-safe.
 * Successful finish returns the real drain report. On INTERNAL_ERROR do not unload.
 * Host must also join its own calling threads and release buffers before unload.
 */
phu_status phu_begin_shutdown(phu_handle handle);
phu_status phu_close(phu_handle handle, phu_buffer* out_report);

/* Reserve immediately before GPU/readback work. Layout JSON:
 * {width,height,row_stride_bytes}. Pixels must be complete composited/downscaled/masked
 * top-down BGRA8888 (alpha ignored). Core validates layout and owns capacity/attribution.
 * OK returns a single-use frame token; INACTIVE means core refused acquisition (including
 * inactive/policy/capacity/invalid-layout/allocation). No time-based token expiry.
 * Carry token through readback; abandon on failure/cancellation/resize. No prefetch queue.
 * Submission COPIES exactly row_stride_bytes*height bytes into the acquired Rust buffer.
 * Every submit attempt consumes the token, including bad length/wrong client/errors.
 * ACCEPTED precedes encoding/hooks/delivery, not their success. Identity/consent/session
 * transitions may return STALE. Manual stop allows valid outstanding frames to submit.
 * Close abandons all host-held tokens; later use returns CLOSED. Abandon is single-use.
 */
phu_status phu_begin_frame(phu_handle handle, phu_bytes layout, phu_frame* out_frame);
phu_status phu_submit_frame(phu_handle handle, phu_frame frame, phu_bytes pixels);
phu_status phu_abandon_frame(phu_frame frame);

#ifdef __cplusplus
}
#endif
#endif
