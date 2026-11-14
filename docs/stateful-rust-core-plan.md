# Stateful Rust core and thin Unreal adapter

The stateful Rust SDK now exists at `b717aef23d4869bdf94e75ce36bbd383d3575c6f`. Unreal integrates its public API through ABI v3. The current responsibility map and integration sequence are in [integration-plan.md](integration-plan.md); validation gates are tracked in [implementation-status.md](implementation-status.md).

## Contract

Rust owns identity, consent, registered/person/group properties, sessions, feature flags, remote config, persistence, event assembly and delivery. Its screenshot API also owns manual recording policy, acquisition attribution, end-to-end capacity, JPEG encoding, hooks and delivery. Unreal owns engine context/signals, UObject lifetime, game-thread dispatch and pixel acquisition/preparation.

The bridge is not a second SDK. It can own handles, buffers, borrowed foreign callbacks, notification plumbing and shutdown accounting. It must use supported public Rust operations rather than private transports or a parallel policy implementation.

### Compatibility boundaries

- The C++/Blueprint surface exposes supported stateful operations and native result shapes.
- Commands returning `Ok` confirm invocation, not successful state mutation or persistence.
- `DrainReport` state/delivery outcomes describe local progress, never ingestion.
- Captures permitted before opt-out may still persist and deliver. Reset restores configured default consent.
- State and event storage belong to Rust under its own namespaces and schema. Storage failure uses the SDK's memory fallback.
- Feature-flag notifications are owned SDK snapshots; they can be superseded by the time the game thread consumes them. They are not a targeting-generation protocol.
- Capture-time SDK `session_id` and `distinct_id` are top-level fields in raw canonical v1 analytics events. Event library properties identify `posthog-unreal/0.0.1`; the transport header separately identifies `posthog-rs/0.31.3`.
- Before-send filtering preserves complete event fields, supports mutation/drop and fails closed. Removed attribution is not refilled by the adapter.

## Replay

Manual producers and the automatic owning-viewport recorder reserve an opaque single-use acquisition before readback, carry it through bounded host work, then submit composited/downscaled BGRA8888 bytes or abandon it. Rust owns the raw lease, attribution checks, encoding and delivery.

Automatic viewport pixels are unredacted, including readable UI. Applications must decide which content is appropriate to record; manual producers can apply redaction before submission. Rendered acceptance covers owning-surface isolation, orientation, pitch/formats, resize/window races, cancellation, bounded staging and teardown. It is not a privacy guarantee.

## Lifetime and shutdown

Creation and blocking waits run on tracked host workers. C++ keeps callback contexts strongly owned through completed native shutdown and all admitted host calls. Callback-origin shutdown cannot wait for itself; `Deferred` requires an outside finisher. Native and host threads must join before code unload. Thread-creation failure must retain ownership rather than abandon executable work.

Initialization cancellation and game-instance replacement serialize namespace reuse. Late UObject delivery is cancelled using owner lifetime, not independently reimplemented SDK targeting/consent generations.

## Remaining production evidence

- Packaged Blueprint consumers and matching ABI/header/artifact builds across supported platforms.
- Rendered stress across embedded/multiple-PIE surfaces, foreground/window transitions and additional HDR/backends beyond the validated Metal fixture.
- Platform-specific filesystem, network, lifecycle and packaged runtime runs.
- Sustained offline/recovery behavior and explicit frame-time/memory/shutdown budgets.
- Exact artifact hashes, dependency provenance and supported toolchains.
- Release/platform review beyond the completed main-aware integrated review and focused expired-session recheck.

Use the canonical SDK specifications and acceptance scenarios for changed behavior. Peer implementations explain platform differences but do not replace the specified contract. Local mock-server evidence must remain distinguished from real ingestion and playback verification.
