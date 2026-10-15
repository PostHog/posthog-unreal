# Integration implementation status

The integration is in development. Native delivery, the stateful C++ client, cached feature flags, game-instance ownership, Project Settings, and application lifecycle capture are implemented. Full Blueprint function/async nodes, flag-change notifications, and the Analytics adapter remain incomplete. This is not a release candidate.

## Implemented

- Runtime plugin module and private external-library build module.
- `UPostHogSubsystem` owns a native client per game instance, with asynchronous setup/close, game-thread completion delegates, explicit editor/server guards, and runtime/editor/PIE namespace separation. Weak native-only ownership records serialize namespace reuse during cancelled initialization and close.
- Blueprint subsystem nodes for Capture, Identify, Get Feature Flag, and Reload Feature Flags, plus typed property builders and value inspection. Explicit reloads publish game-thread completion events, reject stale deferred snapshots, coalesce pending calls, and cancel notifications on owner shutdown/destruction.
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

## Upstream dependency

The bridge pins `posthog-rs` mainline `b0038bde4f8ea39b6440ddc9fde376ac602c5b65` as its baseline. Required embedding additions are being implemented separately on `feat/unreal-embedding` in `~/dev/worktrees/posthog-rs-unreal`.

During development, `POSTHOG_RS_PATH` supplies that checkout as a Cargo patch. The bridge cannot build against unmodified mainline yet. The Cargo lockfile currently records the patched dependency. Before distribution, review and land the Rust additions, replace the pin, regenerate the lockfile without the local patch, and remove the local-build requirement. Do not copy the upstream implementation into this repository.

The Rust additions are opt-in. The plugin selects legacy batch ingestion, disables default features including error interception, and attributes transport requests to `posthog-unreal`.

## Behavioral sources

- `sdk-specs`: `513d94cf9ab8fac7ec5fc1c8e8e21fd88f72f231`.
- Unity: `216d4c162d0d37e2b5216b4a3bcd3a5abdcec3a5`.
- Unreal: installed UE 5.8 headers and toolchain.

Specs take precedence over divergent Unity behavior. Reset clears persisted opt-out as specified while preserving queued event identity and emitting no reset event. Automatic sessions use Unity's 30-minute inactivity and 24-hour maximum, with UUIDv7 IDs; expiration occurs strictly after each boundary. Initial delivery settings follow Unity: threshold 20, interval 30 seconds, queue capacity 1,000, batch size 50, request timeout 10 seconds, and shutdown budget 3 seconds. Embedding caps individual requests by the shutdown budget so already-running requests cannot consume a longer request timeout during close.

## Validation so far

On macOS arm64:

- All 5 Rust bridge unit tests pass.
- Upstream blocking legacy unit suite passes, including consent ordering, shutdown disk-write cutoff, failed-purge recovery, restored queue bounds, exclusive spool locking, and corrupt-record recovery.
- Upstream default-feature Capture v1 unit suite passes; embedding itself rejects Capture v1 configuration.
- All 9 Python integration tests pass, covering exact native event preservation, offline replay, consent purge/restore, queue overflow, concurrent close, stale flags, stalled requests, and two-phase shutdown storage ownership. One of these tests runs the Unreal suite and verifies received wire events.
- UE 5.8 Editor Development builds; all 7 Unreal automation tests pass. Stateful assertions cover identity linking/deduplication, property precedence and null handling, reset/consent restart, hook redaction/reentrancy, consent-generation invalidation, exact session boundaries, person-profile guards, parallel-client isolation, and size/depth persistence boundaries.
- Flag tests cover typed/null/string payloads, partial-response metadata, cache restoration and context mismatch, local-only reads, concurrent reload/exposure deduplication, caller defaults, completion snapshots, targeting setters, failure retention, opt-out/shutdown, and cache-pressure consent persistence. Wire assertions cover automatic flag enrichment and minimal/full exposures through public getters and `BeforeSend`. Rich-response metadata is exercised with normalized export/cache fixtures; the maintained mock server supplies legacy flags responses.
- Subsystem automation uses real standalone game instances with separate client names. It verifies policy guards, independent identity/session state, world replacement without client replacement, deduplicated lifecycle signals, update detection after identity reset/restart, Project Settings setup, and closure of retained native pointers. A controlled native-initialization barrier allows destruction and garbage collection of the old game instance before a new owner reuses the same namespace.
- Blueprint automation invokes the four reflected subsystem methods through `ProcessEvent`, binds a dynamic completion delegate, and checks typed properties, missing/false/variant flags, coalesced and reentrant reloads, stale deferred snapshots, opted-out calls, and shutdown/destruction cancellation. Python assertions verify capture/identify wire data and exposure deduplication. UHT and reflected runtime coverage do not establish interactive graph or Blueprint-only packaged-game support.
- An initial fixture that relabeled standalone contexts as PIE triggered editor play-mode assertions. It was replaced with valid standalone setup/teardown. Actual editor multi-PIE Play/Stop remains an explicit validation gate; standalone results do not establish it.
- Filesystem failure injection confirms failed native purge cannot persist opt-in; restarting with the original spool remains opted out and does not replay those events.
- One full run stopped before editor launch because the mock server exited during startup; the previous runner discarded its startup diagnostics. Startup diagnostic preservation was added, and the next complete run passed. The original startup cause is unconfirmed.
- Read-only review identified persistence bounds, creation/teardown admission, and opt-in acknowledgement defects. The fixes and final purge-before-opt-in ordering received focused rechecks with no remaining findings.

The feature-flag review found missing reload handling for unchanged targeting properties, caller-default support, and completion snapshots. All three were corrected and regression-tested; the focused read-only recheck found no remaining issues.

The subsystem/settings/lifecycle slice received an independent read-only review with no issues found. Setup/shutdown dynamic delegates compile through UHT, but full Blueprint graphs and the interactive Project Settings panel still need consumer/UI validation.

This does not establish Windows, Linux, macOS x64, Shipping, packaged-game, Blueprint-only, multi-PIE, or Live Coding support. The Unreal build currently permits only Mac arm64.

## Delivery and shutdown limits

`ACCEPTED` means bounded memory admission, not disk or server acknowledgement. Checkpoint waits for earlier worker operations and reports storage errors. A flush is one delivery attempt; pending, delivered, terminally dropped, and storage-error counters describe the outcome.

Shutdown closes admission and sets a total work deadline before waiting for admitted calls. Requests use the remaining budget; the worker does not start another event persistence write after expiry. An event accepted only into memory can be lost at the deadline. Previously durable unsent events remain for restart. An already-running filesystem operation and OS scheduling cannot be preempted; shutdown still joins workers rather than abandoning code that could outlive the module.

Opt-out closes local and native admission immediately, schedules state persistence and purge, and acknowledges both asynchronously. Requests already admitted may reach the server. Opt-in keeps the local gate closed until native purge succeeds and the new consent state is written. Coalesced property writes preserve that desired consent. State writes use flushed temporary files and replacement; they do not establish cross-platform power-loss guarantees.

The native shutdown deadline starts before waiting for Unreal state writes, while the handle retains the exclusive spool lock until final close. Background state I/O remains subject to filesystem scheduling. Future completion can run off-thread; UObject consumers must marshal to the game thread.

## Feature-flag limitations

- Rust currently returns malformed serialized payloads as raw strings, indistinguishable from valid decoded string payloads. Strict malformed-payload rejection is deferred. Fix the upstream export's parse-validity information before stable release; previously persisted ambiguous caches may need invalidation. Unreal does not reparse exported strings.
- The mock server's quota-only array response is rejected by the current Rust parser, so it surfaces as a generic load failure with same-context cache retention. Recognized normalized quota responses preserve the cache and return `QuotaLimited`.
- Targeting overrides are explicit; analytics person/group properties are not automatically mirrored. The mock records identity/group membership but not targeting property bodies; targeting serialization is checked through the persisted request context and ABI/source mapping.
- Explicit Blueprint reloads have game-thread completion events. Automatic flag-change notifications (including preload and identity-triggered refresh) remain unimplemented.

## Next work

1. Complete the remaining Blueprint function/async surface and automatic flag-change notifications.
2. Add the Unreal Analytics adapter/provider mode over the same client and coordinate its ownership with the subsystem.
3. Add C++/Blueprint consumer examples and interactive Project Settings validation.
4. Complete spec acceptance coverage, actual editor multi-PIE Play/Stop and level-travel tests, and packaged-game/Blueprint-only consumers. Existing standalone game-instance tests do not establish real PIE behavior.
5. Finish upstream API review, full-suite/MSRV checks, API snapshot, dependency/license evidence, and an immutable released dependency pin.
6. Produce and verify prebuilt artifacts across the declared desktop matrix before release.
