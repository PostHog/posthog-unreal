#ifndef POSTHOG_UNREAL_FFI_H
#define POSTHOG_UNREAL_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PostHog Unreal native ABI. */
#define PHU_ABI_VERSION 1u
#define PHU_MAX_INPUT (1024u * 1024u)

typedef uint64_t phu_handle;
typedef int32_t phu_status;
enum {
    PHU_OK = 0,
    PHU_ACCEPTED = 1,
    PHU_INVALID_INPUT = 2,
    PHU_CLOSED = 3,
    PHU_REQUEST_FAILED = 4,
    PHU_INTERNAL_ERROR = 5,
    PHU_UNSUPPORTED_ABI = 6,
    PHU_DISABLED = 7,
    PHU_QUEUE_FULL = 8,
    PHU_STORAGE_ERROR = 9
};

typedef struct phu_bytes {
    const uint8_t* data;
    size_t len;
} phu_bytes;

typedef struct phu_buffer {
    uint8_t* data;
    size_t len;
} phu_buffer;

/* JSON inputs are copied during the call; strings are UTF-8, not NUL-terminated.
 * Inputs must be readable for len bytes. Outputs must be writable, disjoint
 * from inputs. Invalid non-NULL pointers cannot be validated by the bridge.
 * No error result includes credentials, event data, or remote response bodies.
 */
uint32_t phu_abi_version(void);

/* options: {project_token, host, storage_path, opted_out?, request_timeout_seconds?, shutdown_timeout_ms?,
 * flush_at?, flush_interval_ms?, max_queue_size?, max_batch_size?}.
 * Configuration is explicit; there is no credential or host fallback.
 * Creates HTTP clients/threads: run off the game thread. Returns handle 0 on error.
 */
phu_status phu_create(uint32_t abi_version, phu_bytes options, phu_handle* out_handle);

/* event: {event, distinct_id, uuid, timestamp (RFC3339), properties?}.
 * ACCEPTED means bounded memory-queue admission, not durable/server acceptance.
 * The event is fully prepared: Rust does not add properties or run host hooks.
 * Checkpoint waits for earlier durable writes and reports storage failures.
 */
phu_status phu_capture(phu_handle handle, phu_bytes event);

/* Blocking delivery attempt, NOT a delivery receipt. Run off the game thread. */
phu_status phu_flush(phu_handle handle);

/* context: {distinct_id, groups?, person_properties?, group_properties?}.
 * Blocking remote request; run off the game thread. Returns JSON with flags,
 * request_id, evaluated_at, errors_while_computing, and quota_limited.
 * Missing flags have no map entry; false remains a boolean, variants are strings.
 * Each flag has value, optional payload, and evaluation/exposure metadata.
 * Does not emit exposure events. Consent changes invalidate pending completions.
 * Output is zeroed on error. Release successful output with buffer_free.
 */
phu_status phu_flags(phu_handle handle, phu_bytes context, phu_buffer* out_buffer);

/* Stop admission immediately, then schedule durable purge. Host persists consent.
 * Opt-in waits for purge; checkpoint waits for preceding worker/disk operations.
 * Both opt-in and checkpoint must run off the game thread.
 */
phu_status phu_opt_out(phu_handle handle);
phu_status phu_opt_in(phu_handle handle);
phu_status phu_checkpoint(phu_handle handle, phu_buffer* out_report);

/* Stops new admission, waits for admitted operations, drains/joins upstream.
 * Run off the game thread. Requests are bounded by shutdown_timeout_ms; thread
 * scheduling and filesystem calls are not hard realtime. CLOSED means invalid
 * handle or another caller has
 * started closing it, not that that caller has finished. Host must join all its
 * calling threads before module unload. Never abandon a closing thread.
 * No callbacks cross this boundary. Handles are never reused.
 */
phu_status phu_close(phu_handle handle);

/* Local first phase: closes admission and starts the native shutdown budget.
 * The handle retains its spool lock until phu_close joins and releases it.
 * All operations except phu_close subsequently return CLOSED.
 */
phu_status phu_begin_shutdown(phu_handle handle);

/* Free an unchanged returned buffer exactly once, or pass {NULL,0}. */
void phu_buffer_free(phu_buffer buffer);

#ifdef __cplusplus
}
#endif
#endif
