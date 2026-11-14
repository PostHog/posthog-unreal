# Unreal / stateful Rust integration

## Boundary

Rust owns platform-independent SDK behavior. Unreal supplies engine signals, sampled platform context, presentation, prepared pixels and native-resource lifetime.

```text
Game instance / Blueprint / C++
             |
  Unreal adapter and UObject dispatch
             |
     versioned ABI v3 bridge
             |
    posthog::stateful::Client
             |
  shared Rust storage and delivery
```

The dependency is package `posthog` at immutable revision `b717aef23d4869bdf94e75ce36bbd383d3575c6f`, with default features disabled and `tls`/`session-replay` enabled. The Cargo import alias is `posthog_rs`. The legacy embedding model is superseded by this stateful public API.

## Responsibilities

| Owner | Responsibilities |
| --- | --- |
| Rust SDK | Identity/person/group transitions; registered properties; event assembly; consent; sessions; targeting/cache/exposure policy; remote config; state/event persistence; retries; replay eligibility, attribution, encoding and delivery |
| C ABI | Bounded JSON, owned buffers, panic containment, opaque client/frame handles, callback lifetime accounting, polled flag mailbox and tracked shutdown |
| `FPostHogClient` | Unreal value/status conversion, complete foreign-hook adaptation, strong native-lifetime registry, tracked asynchronous waits and manual frame leases |
| `UPostHogSubsystem` | Game-instance ownership, editor/server guards, initialization cancellation, environment/PIE inputs, sampled context, engine signals, game-thread notification delivery and instance-bound Analytics adapter/binding lifetime |
| `FPostHogAnalyticsProvider` / `PostHogAnalytics` | Explicit world/instance resolution and world-less designation, Unreal attribute conversion/default array, host SessionStart/SessionEnd annotations and factory configuration validation |
| Prepared-pixel caller | Safe acquisition, BGRA conversion/downscaling, any application-provided redaction and submission/abandonment |
| `FPostHogReplayRecorder` | Owning-viewport paint/window association, GPU readbacks, pixel worker and engine-resource retirement |
| `FPostHogReplayWorkState` / `FPostHogReplayWork` | Two host staging permits, exact native-lease ownership and engine-source cancellation |

Neither C++ nor the bridge maintains SDK identity, consent, flags, exposure deduplication or session timeout state. The engine's foreground state gates host acquisition but does not invalidate or reattribute an outstanding Rust frame.

## Dependency checks

`python -m unittest discover -s Tests -p 'test_*.py' -v` includes `test_architecture.py`. It classifies production C++ units under `Source/PostHog` and `Source/PostHogAnalytics` by responsibility, checks their literal include graph for cycles, confines direct ABI includes to the native/composition boundary, and rejects SDK file-persistence dependencies in client/provider adapters. New production units must be classified there. Negative fixtures prove forbidden imports/cycles fail and supported public-entry dependencies pass.

Engine/generated headers, macro/dynamic dependencies and build-rule dependencies are outside that static graph; UHT, compiler checks and semantic review cover those separately. An include check cannot prove that SDK policy has not been reimplemented through an otherwise allowed API.

## Native lifetime

ABI inputs are borrowed for a call. Rust allocates outputs and the caller frees them exactly once. `BeforeSend` receives complete assembled event JSON; callback output uses a Rust allocation and transfers back on every return. Malformed/dropped output fails closed.

C++ retains native lifetime contexts strongly until completed native close, including after a public client or UObject is destroyed. Each admitted host call retains its context through conversion and native execution. Begin close shuts admission before any waits. The finisher joins native work, waits host calls, then releases the hook and registry entry outside locks. Failed thread creation leaves ownership available for final module teardown. Deferred, invalid-handle and error statuses do not certify completion.

Renderer cancellation synchronizes SDK-call admission before native begin-close; late GPU work cannot submit to Rust. A private retirement port in the existing client operation waits for GPU fences, the pixel worker and detached callbacks before final native close. Native-hook/render-thread callers report Deferred rather than waiting for engine-thread work. Plain clients have no renderer port.

Module shutdown stops new creation, begins close for all contexts before retirement waits, drains tracked host tasks and finishes remaining native contexts. It cannot return to unload while executable native work remains. Consumers must also release their own references and caller threads. Dynamic module reload is unsupported.

## Values and outcomes

Commands return invocation status, not invented mutation/deduplication booleans. Capture returns bounded admission. Flush/close report independent persistence and delivery outcomes, not historical queue counters or ingestion acknowledgments. C++ adds adapter availability errors and a joined-shutdown indicator.

Property objects retain their accepted decode/mutation byte bound across native conversion, value/reflected copies and edits. Read results can use the ABI output limit; command and capture arguments still obey the separate input limit.

Flags are read from Rust. The mailbox is a one-slot coalescing presentation queue, not a cache. Notifications include enabled results and their producing context; all-results getters include known false. No C++ targeting generations or request-correlated futures are synthesized.

The subsystem selects `Saved/PostHogStateful` and instance/environment inputs. Before initialization it obtains an opaque host/token/instance coordination key from Rust, using SDK endpoint normalization and trimmed host/token inputs. Equivalent inputs reject a second live engine owner or wait for a cancelled predecessor to finish. Rust owns fresh versioned records and namespace leases; direct native clients may use memory-only operation. Storage failures retain canonical Rust memory fallback.

## Analytics and canonical migration

The Analytics provider retains only its default-attribute array and annotation admission markers. `FPostHogClient::CaptureWithFallback` is a private friend boundary that serializes per-capture `fallback_properties`; Rust fills absent keys after registered/platform/generated properties and typed-group merging, before hooks. No adapter superproperty, identity, consent, session-clock or flag cache is maintained. Explicit properties win; hooks remain authoritative. Array replacement/clear does not mutate SDK registration. Callback-capable operations retain local client/annotation-state references and do not hold adapter mutexes across native hooks.

Unreal `SessionStart`/`SessionEnd` annotate SDK-created sessions. Repeated successful starts deduplicate by SDK ID; in-flight or rejected starts cannot claim admission, and end revalidates SDK identity after callbacks rather than clearing a newer session. Custom `SetSessionID` now returns false without changing SDK state. Registered properties take the canonical SDK precedence over platform context, rather than the prototype C++ context-over-registration order.

Failed flag handling also follows the canonical SDK, not the retired C++ cache. A v2 `failed:true` evaluation is missing, never a healthy false value. It makes the response partial even when top-level `errorsWhileComputingFlags` is false: prior successful values/payloads and their producing metadata are retained, rather than deleted as the old complete-response cache test expected. All-failed responses are partial empty evaluations, not complete-empty replacements. Healthy false remains available; a genuinely successful full empty response replaces the cache, and restored records must match evaluation context. This is a reachable prototype-to-canonical behavior change, not literal parity. Decoder/stateful regressions live in the pinned Rust SDK; Unreal tests cover owned result conversion, known-false/missing/payload reads and reporting over the public ABI.

## Replay sequence

1. Explicitly request recording with the SDK's manual start operation.
2. Reserve an owned Rust acquisition immediately before capture using final dimensions/stride.
3. Carry that token through host work; release it on every failure or cancellation path.
4. Submit complete composited/downscaled BGRA8888 once. Automatic viewport content is unredacted; manual producers supply any required redaction.
5. Rust validates attribution and consent/session transitions, encodes JPEG, runs hooks and delivers with retained attribution.

A valid outstanding acquisition may complete after manual stop. Flush excludes host-held frames. Already submitted work can remain eligible after later consent changes. Replay storage is memory-only.

The subsystem attaches the recorder to its own game instance. A transparent viewport paint observer publishes same-paint window-relative geometry before Slate queues that window's draw; an end-of-draw command expires unused metadata. Only the matching window/backbuffer dimensions can authorize a cropped GPU copy, and native admission occurs before acquisition. Embedded editor/PIE surfaces use the viewport rectangle, not the whole editor window. Unsupported transforms/layouts/formats skip capture.

Two host permits cover GPU, queued and processing work. Each `TPostHogReadbackSlot` reuses its readback only for the allocated crop extent and pixel format; incompatible allocations are replaced after their previous GPU job/fence retires. Ready readbacks have padding removed on the render thread; conversion/downscaling runs on one joined worker. GPU fence waits occur during retirement, not normal capture. Engine-source revisions cancel resize/window/foreground work and do not represent SDK identity or consent. Rust stop can still permit an already acquired frame.

`PostHogReplay.*` owns pixel math; the recorder does not encode JPEG or envelopes. Automatic content is unredacted. Real rendered verification is required before claiming renderer correctness.

## Gates

1. Exact pinned Rust / ABI public-entry tests and lifetime validation.
2. Thin C++/Blueprint source and reflected-header validation.
3. Matching native artifact installation, actual UE consumer build and automation.
4. Automatic owning-viewport acquisition with real rendered source/orientation/resize/retirement evidence.
5. Supported desktop runtime and packaged Development/Shipping validation, performance budgets and independent review.

Native synthetic images prove encoding and wire behavior, not renderer safety, mask discovery, live ingestion or backend playback. See [implementation status](implementation-status.md) and [CI](ci.md).
