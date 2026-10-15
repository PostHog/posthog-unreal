# Integration implementation status

The integration is in development. Native delivery, the stateful C++ client, cached feature flags, game-instance ownership, Project Settings, and application lifecycle capture are implemented. Full Blueprint function/async nodes, flag-change notifications, and the Analytics adapter remain incomplete. This is not a release candidate.

## Implemented

- Runtime plugin module and private external-library build module.
- `UPostHogSubsystem` owns a native client per game instance, with asynchronous setup/close, game-thread completion delegates, explicit editor/server guards, and runtime/editor/PIE namespace separation. Weak native-only ownership records serialize namespace reuse during cancelled initialization and close.
- Project Settings with reflected runtime options, optional automatic setup, and explicit-configuration precedence. Game-thread context sampling includes OS, engine version, locale, and application metadata.
- Core lifecycle and Slate activation observers feed bounded plain-data notifications to the game-thread ticker. Install/update/open/background events follow normal capture, hooks, and consent; persisted version/build markers survive identity reset.
- Versioned C ABI with per-client handles, UTF-8 JSON inputs, explicit buffer ownership, panic containment, and concurrent-close protection.
- Fully prepared event submission through the existing Rust worker, without a second queue or Rust property enrichment.
- Upstream embedding options, durable event-store interface and desktop file spool, memory-admission outcomes, delivery/storage counters, consent epochs, purge/resume, and a shared shutdown deadline.
- Side-effect-free feature-flag snapshot export with values, payloads, and evaluation metadata.
- Typed, local feature-flag getters with caller defaults, asynchronous preloading/reload, completion-time value snapshots, context generations, and persisted targeting overrides. One load runs at a time; context changes queue the latest context.
- Same-context cache retention on failed/partial/quota-limited loads, context-matched restart restoration, bounded optional cache persistence, and per-cache exposure deduplication. Server-gated minimal events are filtered after hooks without restoring removed properties.
- Native artifact build script, lockfile, checksums, source fingerprint, and macOS deployment-target configuration.
- Stateful `FPostHogClient` with asynchronous creation/close, capture/screen, identity, alias, registered/person/group properties, consent, and reset. Rust remains the only event delivery queue.
- JSON-backed reflected property/value types with copy semantics, typed scalars/objects/arrays/null, and size/depth validation. Complete persisted-state bounds include identity and consent metadata.
- Host/token/client-name storage namespaces, coalesced off-thread state writes, temporary-file replacement, and restored identity/consent. Successful opt-in requires native purge and state persistence before local admission opens.
- UUIDv7 event and session IDs, automatic inactivity/maximum-duration expiration, explicit sessionless mode, and host-callable foreground/background transitions.
- Snapshot enrichment and a reentrant C++ `BeforeSend` hook outside state locks. Consent generations invalidate pre-withdrawal snapshots even after opt-in.
- Tracked native tasks, creation/teardown admission locking, and module shutdown that closes clients and joins work. Dynamic module reloading is disabled.
- C ABI/mock-server integration tests and Unreal automation covering native delivery, property values, and the stateful client.

## Implemented (proof of concept)

- Screenshot session replay behind `Replay.bEnabled`. The subsystem owns a recorder that hooks Slate's back-buffer-ready delegate on the render thread. It throttles to the configured interval and copies the main game window into a ring of three non-blocking GPU readbacks. A frame is dropped when no readback is free. One worker thread converts 8-bit, 10-bit, and FP16 back buffers to BGRA8, scales them, and encodes JPEGs. Each frame becomes one `$snapshot` event (a meta event and a full snapshot with one screenshot wireframe) submitted through `phu_capture_replay`. Rust batches the frames per session and sends them to `/s/`.
- Recording requires the local setting, consent, an active session, and a `session_recording` object in the last flags response. That remote value is cached and persisted with the flag cache. The recorder pauses in the background. Opt-out stops admission and drops queued frames. Subsystem shutdown and module teardown stop the recorder, flush rendering, and join its worker before clients close.

Proof-of-concept limitations:

- No masking. Every frame contains everything on screen.
- Only the `session_recording` on/off state is honoured. Endpoint override, `linkedFlag`, `sampleRate`, minimum duration, and event triggers are ignored.
- The replay queue is in memory only. Frames are lost on exit or crash and are never spooled to disk.
- Desktop only. Validated once on macOS arm64 (Metal) in a `-game` Development build of the example against the mock server: frames arrived at the configured 1 Hz, the decoded JPEG showed the game, and `$session_id` matched the analytics events. Measured over a 40-second run at 1280x720 scaled to 960x540: 6-9 ms on the render thread per capture (the readback map and copy, once per interval) and 8-30 ms on the worker per frame (resize and encode). Neither is optimized. Windows, Linux, consoles, mobile, D3D12/Vulkan back-buffer copies, and HDR output are unverified.
- In the editor or PIE, frames show the whole editor window.
- The server returns `session_recording: false` for projects with `recording_domains` unless the SDK is on its allowlist, and `posthog-unreal` is not on it yet.

## Upstream dependency

The bridge pins `posthog-rs` v1 baseline `934113e63b9ad292de45d7db0af53913eb6eb117`. Required embedding additions are being ported separately on `feat/unreal-embedding-v1` in `~/dev/worktrees/posthog-rs-unreal-v1`; they have not landed in this baseline.

During development, set `POSTHOG_RS_PATH` to that checkout to supply it as a Cargo patch. The bridge cannot build against the unmodified baseline yet. The refreshed Cargo lockfile records the locally patched v1 dependency. Before distribution, review and land the Rust additions, replace the pin, regenerate the lockfile without the local patch, and remove the local-build requirement. Do not copy the upstream implementation into this repository.

The Rust additions are behind the optional `embedding` Cargo feature (off by default). The plugin enables `embedding` and `tls` and uses v1 analytics ingestion at `/i/v1/analytics/events`, disables default features including error interception and the async client, and explicitly enables `tls` for HTTPS. Unreal continues to submit prepared events through the existing Rust analytics worker. Rust's v1 conversion moves legacy processing properties such as `$process_person_profile` into event options.

SDK attribution is transport-owned: embedding `sdk_name` and `sdk_version` set `PostHog-Sdk-Info` to `posthog-unreal/<bridge version>`. V1 removes `$lib` and `$lib_version` from wire properties; the server materializes them from that header. `BeforeSend` cannot remove or override those two server-materialized fields. It retains control over custom and context properties, which Rust must not restore after the hook.

## V1 validation gates

`Tests/mock_server_v1_test.py` exercises the maintained mock without loading a native library:

```sh
python3 Tests/mock_server_v1_test.py --mock-server ~/.agents/skills/sdk-mock-server/server.py
```

The mock's `/i/v1/analytics/events` route returns UUID-keyed `ok`, `retry`, `drop`, and `warning` verdicts. Set `analyticsV1Results` through `/__control/state` to a map of event UUIDs to status strings, result objects (with optional `details`), or nonempty sequences of these. Sequences advance per appearance and repeat their last verdict; unconfigured UUIDs return `ok`. `force500.analytics` and `delays.analytics` control this route independently of legacy `batch` controls.

`/__captured/analytics_requests` contains attempted requests: original body bytes as base64 (after HTTP transfer decoding, before content decompression), decoded JSON, selected protocol headers (excluding credentials), and the returned status/results. `/__captured/events` contains mock-materialized v1 events only for `ok` and `warning`; legacy batch capture behavior is unchanged. The mock models SDK attribution, session/window fields, and known processing-option property injection. It does not implement the full backend pipeline, clock-skew correction, quotas, person processing, or downstream enrichment. Its fixture tests are not real-server ingestion evidence.

The native suite separately asserts wire options/session fields, header attribution, hook-minimized properties, partial-verdict retirement, and retry retention after the attempt limit followed by a fresh bounded flush sequence. Build manifests label analytics ingestion as `v1` and record the selected `tls`/`embedding` features.

On macOS arm64 with UE 5.8, the v1 port passes:

- All 8 maintained-mock fixture tests and all 5 Rust bridge unit tests.
- All 11 Python integration tests, including the 6 Unreal automation tests and their wire assertions.
- Native release build and UE Editor Development build.
- Rust full async and blocking suites with `embedding`, plus default-feature unit tests with embedding disabled. Rust formatting, default+embedding clippy, and the feature-inclusive public API snapshot pass. The default-feature public API matches the v1 baseline.

Rust 1.78 validation is blocked by the upstream v1 lockfile: both the unmodified baseline and this port fail to parse `icu_normalizer_data 2.3.0`'s edition-2024 manifest. This port does not raise the advertised minimum Rust version. The new optional `fs2 0.4.3` dependency is MIT/Apache-2.0 licensed; it is absent from the default dependency graph. Cross-platform dependency/license packaging remains a release gate.

## Behavioral sources

- `sdk-specs`: `513d94cf9ab8fac7ec5fc1c8e8e21fd88f72f231`.
- Unity: `216d4c162d0d37e2b5216b4a3bcd3a5abdcec3a5`.
- Unreal: installed UE 5.8 headers and toolchain.

Specs take precedence over divergent Unity behavior. Reset clears persisted opt-out as specified while preserving queued event identity and emitting no reset event. Automatic sessions use Unity's 30-minute inactivity and 24-hour maximum, with UUIDv7 IDs; expiration occurs strictly after each boundary. Initial delivery settings follow Unity: threshold 20, interval 30 seconds, queue capacity 1,000, batch size 50, request timeout 10 seconds, and shutdown budget 3 seconds. Embedding caps individual requests by the shutdown budget so already-running requests cannot consume a longer request timeout during close.

## Previous legacy validation

The following evidence predates the v1 port. Current v1 results are listed above.

On macOS arm64:

- All 5 Rust bridge unit tests pass.
- Upstream blocking legacy unit suite passes, including consent ordering, shutdown disk-write cutoff, failed-purge recovery, restored queue bounds, exclusive spool locking, and corrupt-record recovery.
- Upstream default-feature Capture v1 unit suite passes; embedding itself rejects Capture v1 configuration.
- All 9 Python integration tests pass, covering exact native event preservation, offline replay, consent purge/restore, queue overflow, concurrent close, stale flags, stalled requests, and two-phase shutdown storage ownership. One of these tests runs the Unreal suite and verifies received wire events.
- UE 5.8 Editor Development builds; all 6 Unreal automation tests pass. Stateful assertions cover identity linking/deduplication, property precedence and null handling, reset/consent restart, hook redaction/reentrancy, consent-generation invalidation, exact session boundaries, person-profile guards, parallel-client isolation, and size/depth persistence boundaries.
- Flag tests cover typed/null/string payloads, partial-response metadata, cache restoration and context mismatch, local-only reads, concurrent reload/exposure deduplication, caller defaults, completion snapshots, targeting setters, failure retention, opt-out/shutdown, and cache-pressure consent persistence. Wire assertions cover automatic flag enrichment and minimal/full exposures through public getters and `BeforeSend`. Rich-response metadata is exercised with normalized export/cache fixtures; the maintained mock server supplies legacy flags responses.
- Subsystem automation uses real standalone game instances with separate client names. It verifies policy guards, independent identity/session state, world replacement without client replacement, deduplicated lifecycle signals, update detection after identity reset/restart, Project Settings setup, and closure of retained native pointers. A controlled native-initialization barrier allows destruction and garbage collection of the old game instance before a new owner reuses the same namespace.
- An initial fixture that relabeled standalone contexts as PIE triggered editor play-mode assertions. It was replaced with valid standalone setup/teardown. Actual editor multi-PIE Play/Stop remains an explicit validation gate; standalone results do not establish it.
- Filesystem failure injection confirms failed native purge cannot persist opt-in; restarting with the original spool remains opted out and does not replay those events.
- One full run stopped before editor launch because the mock server exited during startup; the previous runner discarded its startup diagnostics. Startup diagnostic preservation was added, and the next complete run passed. The original startup cause is unconfirmed.
- Read-only review identified persistence bounds, creation/teardown admission, and opt-in acknowledgement defects. The fixes and final purge-before-opt-in ordering received focused rechecks with no remaining findings.

The feature-flag review found missing reload handling for unchanged targeting properties, caller-default support, and completion snapshots. All three were corrected and regression-tested; the focused read-only recheck found no remaining issues.

The subsystem/settings/lifecycle slice received an independent read-only review with no issues found. Setup/shutdown dynamic delegates compile through UHT, but full Blueprint graphs and the interactive Project Settings panel still need consumer/UI validation.

This does not establish Windows, Linux, macOS x64, Shipping, packaged-game, Blueprint-only, multi-PIE, or Live Coding support. The Unreal build currently permits only Mac arm64.

## Delivery and shutdown limits

`ACCEPTED` means bounded memory admission, not disk or server acknowledgement. Checkpoint waits for earlier worker operations and reports storage errors. A flush is one delivery attempt; pending, delivered, terminally dropped, and storage-error counters describe the outcome. V1 retry verdicts and retryable request failures retain their durable records and capacity reservations after the attempt limit. A later flush or timer starts a new bounded sequence in the same analytics worker; normal non-embedding Rust behavior is unchanged.

Shutdown closes admission and sets a total work deadline before waiting for admitted calls. Requests use the remaining budget; the worker does not start another event persistence write after expiry. An event accepted only into memory can be lost at the deadline. Previously durable unsent events remain for restart. An already-running filesystem operation and OS scheduling cannot be preempted; shutdown still joins workers rather than abandoning code that could outlive the module.

Opt-out closes local and native admission immediately, schedules state persistence and purge, and acknowledges both asynchronously. Requests already admitted may reach the server. Opt-in keeps the local gate closed until native purge succeeds and the new consent state is written. Coalesced property writes preserve that desired consent. State writes use flushed temporary files and replacement; they do not establish cross-platform power-loss guarantees.

The native shutdown deadline starts before waiting for Unreal state writes, while the handle retains the exclusive spool lock until final close. Background state I/O remains subject to filesystem scheduling. Future completion can run off-thread; UObject consumers must marshal to the game thread.

## Feature-flag limitations

- Rust currently returns malformed serialized payloads as raw strings, indistinguishable from valid decoded string payloads. Strict malformed-payload rejection is deferred. Fix the upstream export's parse-validity information before stable release; previously persisted ambiguous caches may need invalidation. Unreal does not reparse exported strings.
- The mock server's quota-only array response is rejected by the current Rust parser, so it surfaces as a generic load failure with same-context cache retention. Recognized normalized quota responses preserve the cache and return `QuotaLimited`.
- Targeting overrides are explicit; analytics person/group properties are not automatically mirrored. The mock records identity/group membership but not targeting property bodies; targeting serialization is checked through the persisted request context and ABI/source mapping.
- Automatic game-thread notifications and Blueprint load events remain part of the subsystem integration.

## Next work

1. Add the full Blueprint function/async surface, typed value builders, and flag-change notifications.
2. Add the Unreal Analytics adapter/provider mode over the same client and coordinate its ownership with the subsystem.
3. Add C++/Blueprint consumer examples and interactive Project Settings validation.
4. Complete spec acceptance coverage, actual editor multi-PIE Play/Stop and level-travel tests, and packaged-game/Blueprint-only consumers. Existing standalone game-instance tests do not establish real PIE behavior.
5. Finish upstream API review, full-suite/MSRV checks, API snapshot, dependency/license evidence, and an immutable released dependency pin.
6. Produce and verify prebuilt artifacts across the declared desktop matrix before release.
