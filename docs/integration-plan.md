# PostHog Unreal Engine integration plan

Status: implementation in progress. The C ABI, Rust durable-delivery extension, stateful C++ client, typed properties, cached feature flags, game-instance subsystem, Project Settings, lifecycle capture, and UE 5.8 macOS arm64 runtime tests are implemented locally. Core Blueprint nodes and the explicitly bound Analytics adapter/provider are implemented. Real Blueprint graphs, multi-PIE lifecycle, and Blueprint-only Development/Shipping consumers pass on UE 5.8 macOS arm64. The remaining Blueprint surface, automatic flag-change notifications, and cross-platform/prebuilt-plugin consumer validation remain. See [implementation status](implementation-status.md) for validation and upstream dependencies.

## Recommendation

Build an Unreal plugin with one instance-based, stateful C++ client. Expose it through a game-instance subsystem, PostHog Blueprint nodes, and an `IAnalyticsProvider` adapter. Reuse `posthog-rs` behind a small C ABI for event delivery and remote feature-flag requests.

Use **stateless** to mean that Rust does not own the current user, groups, or session. It already owns operational state: queues, retries, HTTP clients, workers, and flag-exposure tracking. Keep a single delivery queue in Rust rather than putting a second Unreal queue in front of it.

This requires some upstream Rust work. A thin wrapper around today's server client is sufficient for a capture demonstration, but not for Unity-like offline delivery, consent withdrawal, or safe Unreal module teardown. Validate those requirements before committing to Rust as the production base.

## 1. Scope

### Initial beta

- Stateful analytics: capture, screen events, identify, reset, alias, groups, person properties, and registered properties.
- Persistent anonymous identity and consent; bounded, durable event delivery.
- Session tracking and configurable application lifecycle events.
- Remote feature flags, typed results and payloads, cached reads, refresh notifications, and exposure tracking.
- C++, Blueprint, and Unreal Analytics entry points over the same client implementation.
- Project Settings, explicit runtime setup, an example project, and packaged plugin artifacts.

### Later work

Session replay, native crash reporting, automatic log capture, surveys, tracing, and automatic UI/gameplay capture need separate designs. Unreal's `RecordError` can initially remain an ordinary analytics event; it is not a native crash-reporting integration.

Start the compatibility validation on UE 5.8, whose headers are available for inspection. Proposed first release targets are Windows x64, macOS arm64/x64, and Linux x64. Each becomes supported only after editor and packaged-game validation. Agree the minimum UE version before publishing. Mobile and consoles require separate platform/toolchain validation; Rust support alone does not establish Unreal plugin support.

## 2. What the existing code establishes

### Unreal

- `IAnalyticsProviderModule::CreateAnalyticsProvider` is the provider factory used by `FAnalytics`.
- `IAnalyticsProvider` covers events, user IDs, session IDs, flushing, and default event attributes. It does not cover PostHog consent, groups, person profiles, or feature flags.
- `FAnalytics::GetDefaultConfiguredProvider()` invokes the factory on each call; it does not supply a game instance or world.
- Epic's `UAnalyticsBlueprintLibrary` calls that default-provider factory for its nodes. Its attribute wrapper is string-valued.
- Native `FAnalyticsEventAttribute` distinguishes ordinary strings from JSON fragments through `IsJsonFragment()`. Preserve that distinction instead of guessing types from strings.
- `UGameInstanceSubsystem` has the game instance's lifetime, which fits analytics across level travel and permits separate clients in multi-instance PIE.

### Unity

Unity is the reference for the stateful experience: anonymous identity, `Identify`, `Reset`, groups, super properties, sessions, cached flags, and lifecycle integration. Its static facade forwards to a stateful implementation with separate identity, session, storage, queue, and flag components.

Match the applicable client contracts in `PostHog/sdk-specs`, not every Unity implementation detail. For example, the current capture spec defines caller-wins property precedence, and setup now names the credential `ProjectToken`. Those rules are newer than parts of Unity's current API.

### Rust

The inspected local `origin/main` has explicit-identity events, batching/retries, blocking and async clients, immediate-send results, and remote flags. It does not expose a C ABI, a durable queue backend, or a runtime purge/consent barrier. `capture()` and `flush()` do not return delivery receipts. Shutdown drains rather than discards, and its timeout is not a hard bound on all work already underway.

The local Rust checkout is a different development branch: it renames the crate to `posthog` and uses a different ingestion baseline. Plan against the inspected mainline `posthog-rs` package, pin a reviewed version/commit during native validation, and do not accidentally depend on that branch. Mainline defaults to legacy ingestion and gates Capture v1 behind a feature; adopting v1 is a separate compatibility decision.

## 3. Architecture and ownership

```text
PostHog Blueprint nodes / UPostHogSubsystem / C++ callers
                              |
                              v
IAnalyticsProvider --> FPostHogClient (stateful)
       adapter                |
                              v
             private C ABI adapter, explicit handles
                              |
                              v
          posthog-rs delivery and remote-flags client
                              |
                              v
                           PostHog
```

The analytics adapter forwards into `FPostHogClient`; it does not submit independently to Rust.

| Responsibility | Owner |
| --- | --- |
| Current identity, groups, person-profile mode, registered properties | `FPostHogClient` |
| Sessions, app context, lifecycle policy, consent | Unreal client and lifecycle integration |
| Identity/consent/property/flag-cache persistence | Unreal state store |
| Event UUID, capture-time identity/session/timestamp snapshot | Unreal client, using shared Rust primitives where useful |
| Final client enrichment and `BeforeSend` | Unreal client |
| Event queue, durable spool, batching, retry schedule, acknowledgement/removal | Rust delivery worker |
| HTTP, endpoint handling, wire encoding, response parsing | Rust |
| Flag cache, identity-generation checks, exposure deduplication | Unreal client |
| UObject lifetime, reflected APIs, game-thread notifications | Subsystem and Blueprint layer |

There must be one enrichment pass, one event delivery owner, and one exposure tracker. Rust must not silently add back properties removed by `BeforeSend`, overwrite Unreal SDK attribution, or produce a second exposure event.

Serialize native state mutations and capture snapshots so each event sees a complete identity/consent transition. Engine and UObject context is sampled on the game thread and passed as plain data. Native capture may run on other threads without touching UObjects; hooks run outside state locks under a documented reentrancy policy, with consent rechecked before queue admission. Network, flag refresh, and disk work must not hold the state lock or block the game thread.

### Client lifetime and Unreal's default provider

Recommended primary mode: **game-instance-owned clients**.

- `UPostHogSubsystem : UGameInstanceSubsystem` owns a shared native client and an adapter for that client.
- PostHog nodes resolve the subsystem using `WorldContextObject`. No process-global current-world lookup.
- C++ integrations obtain that instance's `IAnalyticsProvider` directly from the subsystem.
- Level travel does not recreate the client or rotate identity. PIE instances have isolated state and storage namespaces.
- A game instance represents one ambient analytics identity, not every player connected to a server. Dedicated-server auto-initialization is disabled by default. Multi-user server capture and split-screen identities need explicit per-user clients or an agreed explicit-identity API, not repeated `SetUserID` calls on a shared singleton.

Compatibility path: **explicitly bound, engine-configured provider** for existing Analytics integrations and Epic's Blueprint library.

- A `PostHogAnalytics` module implements `IAnalyticsProviderModule` and resolves the adapter explicitly designated by `UPostHogSubsystem::BindAnalyticsProvider`. Repeated factory calls return the same adapter; the game instance remains the client/lifecycle owner.
- The designation is process-wide because Epic's nodes do not receive a world. It does not provide per-caller multi-PIE isolation. A second live owner cannot replace the binding; unbind explicitly before selecting another owner.
- World-aware callers resolve World -> GameInstance -> PostHogSubsystem -> adapter. Never select an arbitrary world or create a second client for the configured provider.
- Bind only after setup succeeds, on the game thread. Shutdown/destruction clears the weak binding. Retained adapters stay attached to their original client and close with it; they never retarget replacement owners.
- Provider selection uses `[Analytics] ProviderModuleName=PostHogAnalytics`. Initialize credentials and options through subsystem setup. The provider factory rejects non-empty `ProjectToken`, `Host`, and `ClientName` values from INI/configuration delegates: it cannot reconfigure an already-owned client.
- Bind before creating an AnalyticsMulticast provider, which retains its child adapters. Binding itself never changes the engine's configured provider.

## 4. Public surfaces

Names below are proposed, not a frozen API.

### C++ and PostHog Blueprints

| Area | Proposed operations |
| --- | --- |
| Setup/control | `Setup`, `IsInitialized`, `FlushAsync`, `ShutdownAsync` |
| Analytics | `Capture`, `Screen` |
| Identity | `Identify`, `Reset`, `Alias`, `GetDistinctId`, `GetAnonymousId` |
| Properties | `Register`, `Unregister`, `SetPersonProperties`, `Group` |
| Consent | `OptIn`, `OptOut`, `IsOptedOut` |
| Sessions | `GetSessionId` |
| Flags | `GetFeatureFlag`, `IsFeatureEnabled`, `GetFeatureFlagPayload`, `ReloadFeatureFlagsAsync`, flag-change notification |
| Flag targeting | Set/reset person and group properties for flags |

- Keep Blueprint and C++ behavior identical. Blueprint convenience functions delegate rather than maintain state.
- Provide a typed properties wrapper with string, boolean, number, object, and array builders, plus an explicit validated JSON conversion. Do not make `TMap<FString, FString>` the only PostHog properties API.
- Prototype the reflected JSON/value representation with Unreal Header Tool before freezing it; recursive reflected structures and nested containers need care. Cover copying, GC, packaging, Unicode, and invalid values.
- A feature-flag result distinguishes missing, boolean false, boolean true, and a string variant. Payload absence is distinct from valid JSON scalar/object/array payloads.
- Flag reads that record exposure are `BlueprintCallable`, not pure nodes: Blueprint evaluation can otherwise trigger repeated side effects.
- Async Blueprint actions use game-thread completion delegates and weak lifetime references. Cancellation/owner destruction must not leave latent actions calling destroyed objects.
- `Identify` and `Reset` update local state before returning and schedule flag refresh. Awaitable refresh is explicit, so identity transitions do not wait for network I/O. Document this Unreal adaptation of Unity's async methods.
- Initially expose `BeforeSend` as a native hook with a documented thread contract. Do not call Blueprint code from a Rust worker.

### Unreal Analytics mapping

| Unreal interface | Proposed PostHog behavior |
| --- | --- |
| `RecordEvent` | `Capture` with typed native attribute conversion |
| Immediate record mode, where present | Request prompt asynchronous delivery; never block the game thread |
| `SetUserID` / `GetUserID` | Valid non-empty ID maps to `Identify`; read returns current distinct ID. Empty IDs are rejected; logout uses explicit `Reset`. |
| `FlushEvents` | Trigger the same asynchronous flush; the void API cannot report server acceptance |
| `StartSession` | Explicit session start, idempotent while active; honor the interface's start-event/attribute contract |
| `EndSession` | End the explicit session and request flush, without shutting down the client |
| `GetSessionID` / `SetSessionID` | Read/set the same session manager; accept only IDs compatible with PostHog ingestion, otherwise return false |
| Default event attribute setters/getters | Replace/read an adapter-default layer; do not erase separately registered PostHog properties |
| Purchase, currency, progress, error, demographic helpers | Initially retain Unreal's documented generic event mappings through `RecordEvent`; no inferred revenue schema or crash report |

The adapter shares the client's configured session mode. In explicit mode, events before `StartSession` or after `EndSession` remain sessionless. Automatic mode retains PostHog's normal session rotation. Explicit adapter calls emit ordinary `SessionStart`/`SessionEnd` events, distinct from application lifecycle events; repeated starts in an active session and repeated ends without a session are suppressed. Session transitions and event snapshots use the native state lock; hooks run after unlocking.

For attribute collisions, use adapter defaults below registered properties and explicit event attributes, subject to the client spec's protected identity/person-processing keys. Parse only values marked as JSON fragments. A string such as `"00123"` stays a string. Invalid fragments are logged and omitted without rejecting unrelated event properties.

Test the provider both directly and beneath `AnalyticsMulticast`. Do not take over Unreal Editor telemetry or replace another application's provider implicitly.

## 5. Stateful behavior

### Identity, events, and storage

- Generate and persist a random anonymous ID; do not use a hardware identifier. Rust's `Event::new_anon()` generates a new identity per event and is not the client identity store.
- `Identify` follows anonymous-to-identified linking, same-ID suppression/property updates, and person-processing rules from the client spec. Never alias two known accounts merely because `SetUserID` changed.
- `Reset` creates a fresh anonymous context, clears user-scoped groups/properties/flag state, and rotates the session. Already queued events keep their original identity and remain queued. Reset is not queue purge.
- Freeze identity, groups, session, timestamp, and UUID at capture, not at flush. Preserve them through persistence and retry.
- Apply current client property precedence: registered properties, SDK context, caller properties, then protected person-processing state and documented SDK-owned keys. Run `BeforeSend` on the complete event before durable storage. Apply the spec's null-object-property cleanup without compacting arrays.
- Default SDK attribution is `posthog-unreal` and the plugin version. Correct Rust-generated transport headers and version fields as well as `$lib`. Set client rather than server behavior in Rust.
- Store versioned state under a writable Unreal saved-data location, namespaced by project/host/client instance. Isolate editor/test storage from packaged-game storage. Use atomic writes and recover from corruption or unwritable storage without crashing gameplay.
- Restore consent before starting workers, flag preloads, or lifecycle events. Keep consent/identity state separate from the durable event spool so reset cannot accidentally delete pending events.

### Sessions and lifecycle

Use Unity's 30-minute inactivity and 24-hour maximum session duration as proposed defaults, with UUIDv7 session IDs and an injectable clock for tests. Do not equate a PostHog session to an Online Subsystem multiplayer match or a map load.

Wire supported foreground/background and exit delegates to the owning client. Backgrounding checkpoints durable state and requests a bounded flush; map travel does neither identity reset nor teardown. Lifecycle capture must have one owner, including when both PostHog and Analytics APIs are used.

Proposed settings include `ProjectToken`, `Host`, `PersonProfiles`, lifecycle capture, flag preload/exposure controls, flush threshold/interval, queue and batch bounds, request/shutdown deadlines, initial consent, and debug logging. Use `IdentifiedOnly` as the proposed person-profile default. Agree remaining defaults at the API gate; do not inherit Rust server defaults accidentally. Editor capture and dedicated-server initialization require explicit enablement.

### Consent and shutdown

- Opt-out closes event and flag-request admission immediately, persists the decision, purges queued/retry/spooled analytics data, and invalidates pending flag completions.
- The delivery worker must observe a consent barrier before starting another request/retry. Cancellation cannot retract a request that already reached the server; document that limit accurately.
- Opt-in resumes under the current context without resurrecting purged events.
- Shutdown is a separate operation: stop admission, attempt delivery within a total budget, retain permitted unsent durable events, stop workers, and release handles. Never use today's draining Rust shutdown to implement opt-out.
- UObject teardown and module unload are distinct. No Rust task, callback, or worker may outlive the code it calls. A timeout followed by an abandoned thread is not a safe unload strategy.
- Make shutdown idempotent and all post-shutdown entry points harmless. Exercise repeated PIE start/stop. Do not promise Live Coding/hot-reload support until the native lifetime tests pass.

**Reset and consent:** follow `sdk-specs` reset behavior, including clearing persisted opt-out. Preserve queued events with their original identity. Opt-out is the explicit collection/purge operation; reset is the identity/logout operation. Unity is a reference for platform behavior, not an override where its implementation differs from the spec.

### Feature flags

Fetch flags asynchronously with explicit identity, groups, and targeting properties. Use only the project token in distributed games; local evaluation requiring a secret key is out of scope.

Unreal owns a context-scoped cache and generation counter. Identity/reset/group/targeting changes invalidate the relevant context; stale completions cannot install another user's flags or publish callbacks after opt-out/shutdown. Refresh errors may retain previously valid values only for the same context. Getters never block on network requests.

Expose values, payloads, and evaluation metadata from Rust without recording exposure while serializing/cache-loading. Rust's snapshot getters can emit `$feature_flag_called`; enumerating those getters is not a side-effect-free cache export. Add an explicit export path, and route actual exposure events once through the enriched Unreal client, including deduplication and server-controlled minimal-event rules from the spec.

## 6. Rust embedding work and C ABI

Prefer targeted additions to `posthog-rs`, with an Unreal-specific bridge crate in this repository. Keep Unreal types and lifecycle policy out of the general Rust SDK. Do not create a permanent fork or copy private protocol code into the plugin.

| Required capability | Current gap / planned work |
| --- | --- |
| Native embedding | Versioned C ABI, explicit handles, build artifacts, panic/error containment |
| Honest enqueue result | Distinguish accepted, disabled, invalid, closed, and queue-full outcomes; distinguish memory acceptance from durable acknowledgement |
| Durable delivery | Add a reviewed storage seam to the existing queue, restoring fully formed events and removing only acknowledged or terminally dropped entries |
| Consent barrier | Pause admission/sending, purge buffer/retries/spool, invalidate prepared work, and resume explicitly |
| Bounded shutdown | Deadline-aware work and safe joining, including an already-running request or multi-batch flush |
| Wrapper attribution | Configurable SDK identity and fully prepared-event semantics; no post-hook re-enrichment that restores removed properties |
| Flags export | Side-effect-free values/payloads/metadata with no duplicate Rust exposure capture |

The exact persistence seam belongs in native validation: a Rust-owned spool using an Unreal-supplied directory may be enough for the first desktop targets; platform storage adapters may be necessary later. In either case Rust owns queue acknowledgement and replay, rather than Unreal replaying events into a second opaque in-memory queue.

C ABI rules:

- Opaque per-client handles; no global Rust client. Versioned options/results and a runtime ABI-version check.
- UTF-8 pointer-and-length inputs, bounded JSON values, documented copying, and explicit Rust-owned buffer release functions. No Unreal/Rust container layouts across the boundary.
- Status codes and sanitized diagnostics rather than exceptions. Catch recoverable Rust panics within the bridge when built for unwinding; never unwind across C. Do not claim recovery from allocation aborts or process faults.
- Capture is a bounded local operation. Flush, flag loads, and shutdown run off the game thread; completions are marshalled safely back to it.
- Serialize handle close against concurrent calls; define what cancellation completion means. Never touch a UObject from Rust.
- Disable unneeded default features, including Rust error interception. Select TLS/crypto/compression deliberately and validate their native dependencies.

**Fallback if the queue/lifecycle additions are too costly:** expose a supported request-only Rust API and move queue/persistence/retry scheduling to Unreal. That is a different design requiring approval, not an extra queue around the existing client. Current immediate-send methods still have retry and client-lifecycle behavior; they are not automatically that API. A pure Unreal HTTP implementation is another option, but duplicates protocol maintenance and should be chosen only after the Rust feasibility result.

## 7. Packaging

Proposed repository structure:

```text
PostHog.uplugin
Source/
  PostHog/                 # Client, subsystem, IAnalyticsProvider adapter, Blueprint API
  PostHogAnalytics/        # Configured-provider module/factory
  PostHogEditor/           # Editor-only integration if settings customization needs it
  ThirdParty/PostHogRust/  # External Build.cs, generated C header, artifact lookup
rust/posthog-unreal-ffi/   # Small bridge crate; pinned upstream dependency
Tests/                    # Native/FFI and automation tests
Examples/PostHogExample/  # C++ and Blueprint usage in a packaged-test project
scripts/                  # Reproducible native builds and plugin packaging
```

Keep the native analytics adapter in the runtime module so the subsystem can expose it directly. The configured-provider module depends on the runtime module, not the reverse; the runtime module owns the shared adapter and explicit binding. This avoids a module-dependency cycle.

Start with Rust static libraries, linked once through the runtime module and Unreal's external-module mechanism. Validate Windows CRT compatibility, macOS architecture/deployment targets, Linux toolchain/sysroot requirements, symbol collisions, and Shipping link settings. Reconsider dynamic libraries only with evidence; they add runtime staging and loading requirements.

Distribute verified prebuilt native artifacts so ordinary plugin consumers do not need Cargo. Keep reproducible source builds, lockfiles, checksums, dependency licenses, and a bridge/core version manifest. Validate Blueprint-only consumer packaging as well as a C++ project. Unsupported targets fail clearly during the build rather than silently disabling analytics.

Keep Epic headers, engine binaries, and platform SDKs out of the public repository/artifacts. Use properly provisioned Unreal CI runners for engine-dependent builds; ordinary Rust and ABI tests can run separately.

## 8. Implementation sequence and gates

### Phase 0 — Decide ownership and prove native feasibility

1. Agree the ownership modes, platform/engine floor, initial feature scope, defaults, and reset/consent behavior.
2. Pin mainline Rust and the ingestion mode; record dependency versions and upstream changes required.
3. Build a minimal bridge and load it in UE 5.8. Send one explicitly attributed event to the SDK mock server, fetch flags, and destroy the client.
4. Prove a durable event round trip and a purge/close prototype, including stalled requests and retry backlogs. Measure native binary size, thread count, and capture/game-thread cost.
5. Confirm Rust queue extension versus the request-only alternative before production implementation.

**Exit:** a reviewed ownership/ABI decision, captured wire evidence, and passing create/capture/flush/flags/close tests on the first target. A successful capture alone does not clear the lifecycle gate.

### Phase 1 — Native foundation and delivery

Implement the agreed Rust embedding changes upstream and the small bridge downstream. Add artifact production and the plugin's external module. Establish queue bounds, durable acknowledgement, restart replay, consent purge, error reporting, and bounded teardown tests before adding user-facing integrations.

**Exit:** standalone C/FFI harness passes on each proposed desktop target; no duplicate queue, leaked worker, or callback after destruction.

### Phase 2 — Stateful Unreal client

Implement identity, registered properties, groups, person processing, sessions, state storage, enrichment, `BeforeSend`, consent, and lifecycle integration. Add game-instance ownership and disabled/editor/server guards. Port applicable client acceptance scenarios from `sdk-specs` and record intentional differences for review.

**Exit:** anonymous → identify → reset, restart, multi-PIE isolation, and offline replay preserve exact event attribution; capture performs no network I/O on the game thread.

### Phase 3 — Unreal Analytics and Blueprints

Add typed reflected APIs, async action nodes, the adapter, explicit-session behavior, configured-provider mode, Project Settings, and C++/Blueprint examples. Test both lookup paths and mixed calls on the same client.

**Exit:** Epic's Analytics nodes work with the configured provider; PostHog nodes work with isolated game instances; both produce equivalent analytics without duplicate identity, sessions, or lifecycle capture.

### Phase 4 — Feature flags

Add context-scoped caching, refresh/generation handling, payloads, targeting properties, Blueprint completion events, and deduplicated exposure capture. Preserve detailed response metadata through the Rust boundary.

**Exit:** false/missing/variant/payload cases, stale-response races, opt-out, reset, and reload failures behave correctly; preloading does not emit exposure events.

### Phase 5 — Beta readiness

Package and test editor, Development, and Shipping consumers across the declared matrix. Test installation on a machine without Rust, including Blueprint-only packaging. Finish API/docs/examples, defaults, license notices, release automation, and a public-API snapshot.

**Exit:** all matrix entries have evidence; unsupported platforms/features are accurately documented; approved API/spec deviations and remaining limitations are listed before release.

## 9. Required validation

Use the maintained SDK mock server for deterministic wire inspection and failure injection. Add:

- Client/adapter/Blueprint comparisons for capture, identity, defaults, sessions, consent, and flush.
- Identity transitions with queued events; repeated identification; account switching; reset while delivery is pending.
- Two PIE clients with different identities and simultaneous flags; level travel; repeated play/stop; editor shutdown.
- Offline restart and replay with original UUID/timestamp/context; bounded queue overflow; corrupt spool/state; disk-full and permission failures.
- Slow DNS/TLS/HTTP, connection failure, terminal responses, retryable responses, and Retry-After. Match the selected Rust/spec response classification rather than assuming every 429 is retryable.
- Opt-out before setup, during queued work, during retry backoff, and during an in-flight request; opt-in and restart must not revive purged events.
- Missing versus false flags, variants, malformed payloads, out-of-order responses, deduplication, and consent/identity changes during refresh.
- Typed properties, nested values, null cleanup, Unicode, invalid JSON, metadata precedence, and hooks that remove sensitive fields.
- FFI invalid input, panic containment, concurrent close/capture, and destruction with work outstanding. Run sanitizers where supported.
- A packaged-game smoke test through public APIs, not only a Rust harness or an editor build. Real PostHog ingestion/UI verification is a separate, credential-controlled release check.

Flush completion means the defined delivery attempt has completed, not that PostHog has displayed the event or that delivery is exactly once. Expose and test meaningful outcomes rather than returning unconditional success.

## 10. Implementation decisions and release gates

1. **Rust commitment:** the existing delivery worker now has opt-in durable storage and consent/close control. The Unreal client submits prepared events without a second delivery queue. Upstream changes still need to land and receive an immutable dependency pin.
2. **Ownership:** game-instance-owned clients for both APIs, with an explicit process-wide adapter binding for stock Analytics nodes. Do not promise per-caller multi-PIE isolation for the latter.
3. **Public contracts:** initial engine/platform support, automatic versus explicit sessions and their event names, defaults, reset/consent semantics, and the typed Blueprint properties/result surface.
4. **Release scope:** analytics plus remote flags for beta; crash capture and replay follow separately.

The next ready work is the remaining Blueprint surface and automatic flag-change notifications, maintained consumer examples, and manual Blueprint/Project Settings UI validation. Real Blueprint, multi-PIE lifecycle, and Development/Shipping packaged-consumer checks pass on UE 5.8 Mac arm64; they do not clear cross-platform or prebuilt-plugin installation gates.

## Sources and inspected baselines

Research date: 2026-10-14. Local remote-tracking refs were inspected without fetching; refresh them before implementation.

- Unreal Engine 5.8 installed headers/source: `Analytics/Public/Interfaces/IAnalyticsProvider.h`, `IAnalyticsProviderModule.h`, `Analytics.h`, `AnalyticsEventAttribute.h`; `AnalyticsBlueprintLibrary.cpp`; `Subsystems/GameInstanceSubsystem.h`. These establish the actual interface and world-context constraints.
- [Epic: FAnalytics](https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Analytics/FAnalytics), [subsystems](https://dev.epicgames.com/documentation/en-us/unreal-engine/programming-subsystems-in-unreal-engine), and [third-party library integration](https://dev.epicgames.com/documentation/en-us/unreal-engine/integrating-third-party-libraries-into-unreal-engine).
- [Unity mainline at `55b0023`](https://github.com/PostHog/posthog-unity/tree/55b002388a02a6cd2a7bef637b5a8663bdad256f): `PostHog.cs`, `PostHogSDK.cs`, `PostHogConfig.cs`, identity/session managers, and their changes from the local checkout at `1d4b1a8`.
- [Rust mainline at `b0038bd`](https://github.com/PostHog/posthog-rs/tree/b0038bde4f8ea39b6440ddc9fde376ac602c5b65): `Cargo.toml`, `src/client/{mod,blocking,transport,common}.rs`, `src/event.rs`, `src/feature_flag_evaluations.rs`, and `src/endpoints.rs`. The separate local checkout at `e236766` was also inspected; its APIs/package identity are not treated as mainline.
- [SDK specs at `513d94c`](https://github.com/PostHog/sdk-specs/tree/513d94cf9ab8fac7ec5fc1c8e8e21fd88f72f231): capability inventory and client contracts, especially setup, capture, identify, reset, consent gating, sessions, storage, batching, flags, before-send, flush, and shutdown. Check their current revisions when implementing each phase.
