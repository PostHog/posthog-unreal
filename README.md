# PostHog Unreal SDK

PostHog analytics, feature flags and viewport/manual screenshot replay for Unreal Engine. **In development; not production-ready.**

The plugin is a thin engine adapter over the pinned stateful Rust SDK. Rust owns identity, properties, consent, sessions, feature flags, remote configuration, persistence and delivery. Unreal owns engine signals, value conversion, game-instance lifetime and game-thread delegates.

See [implementation status](docs/implementation-status.md), [architecture](docs/integration-plan.md) and [validation](docs/ci.md).

## Build

The development target is UE 5.8 with macOS arm64, Windows x64 or Linux x64. Source builds need Rust, Python 3.11+ and the platform's Unreal toolchain. Cargo resolves the immutable SSH Git dependency in `rust/posthog-unreal-ffi/Cargo.toml`; configure read access to GitHub. No local Rust checkout is required.

```sh
export UE_ROOT=/path/to/UE_5.8
python3 scripts/build-native.py aarch64-apple-darwin --test
python3 scripts/build-test-host.py
```

The native builder installs a static library and provenance manifest under `Source/ThirdParty/PostHogRust/lib/`. The host builder refreshes the managed plugin copy in `Tests/Unreal/Plugins/PostHog`. Rebuild the library and plugin together: the native interface is **ABI v3**. Use target `x86_64-pc-windows-msvc` on Windows or `x86_64-unknown-linux-gnu` on Linux.

`Tests/Unreal` is the authoritative asset-free SDK automation and rendered test host. `Examples/PostHogExample` retains the playable runner demo and its example-specific tests; it is not required for the SDK test host. `scripts/build-example.sh` refreshes its managed plugin copy before building the legacy Editor consumer, and `scripts/run-example.sh` launches it. Do not put an entire SDK checkout inside a consumer's `Plugins` directory.

## Game-instance setup

Configure **Project Settings → Plugins → PostHog**, or disable automatic setup and call **Setup** on the **PostHog Game Instance Subsystem**. Bind **On Setup Completed** before starting. Setup's boolean means initialization was accepted; completion means a native instance exists. Inspect **Is Disabled** separately: the Rust SDK can initialize disabled after invalid configuration or local startup failure. Readiness does not wait for network access.

Editor/PIE and dedicated-server setup require explicit opt-in. Runtime options do not inherit credentials from Project Settings. Keep credentials out of source-controlled configuration.

The subsystem samples platform/app context on the game thread and supplies foreground/background signals to Rust. It preserves the client through world changes, and closes admission on game-instance teardown. Replacement owners wait off-thread for cancelled initialization and completed native close before reusing the same namespace.

**Capture Application Lifecycle Events** (`bCaptureApplicationLifecycleEvents`, default `true`) captures **Application Opened** with `from_background=false` after foreground setup, **Application Backgrounded** on entering background, and **Application Opened** with `from_background=true` on return. Initial background setup waits for a foreground transition; duplicate engine notifications are coalesced. Disabling this option leaves session/renderer signals active. These events use normal Rust capture, including consent and BeforeSend. Direct native clients do not observe engine lifecycle signals.

Subsystem storage defaults to `Saved/PostHogStateful`. Rust derives its host/project/instance namespace and owns the files. Runtime, editor standalone and PIE inputs are isolated. Concurrent game instances need distinct `ClientName` values. Direct native clients can leave `StorageRoot` empty for memory-only operation. Storage failures use the Rust SDK's memory fallback.

## Analytics and properties

Blueprint exposes **Capture**, **Identify**, **Reset**, **Opt In** and **Opt Out**. `Accepted` means bounded memory admission, not persistence or ingestion. `Ok` on a local command means the SDK method was invoked; it can still no-op, such as a duplicate identify or an unavailable operation. Before setup and after close, operations return `Closed`.

Construct typed properties using **Make String/Boolean/Number/Object/Array/Null Value** and **Set Property**, or parse a JSON object. Copies own their values. JSON null remains distinct from an invalid value.

Consent changes affect new collection immediately. Previously permitted work can still persist, retry and deliver. Reset creates fresh anonymous state and restores configured default consent. Do not use reset as an opt-out operation.

For native C++:

```cpp
FPostHogConfig Config;
Config.ProjectToken = ProjectTokenFromYourConfiguration;
Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogStateful");
Config.ClientName = RuntimeInstanceName;
auto Initialization = FPostHogClient::CreateAsync(MoveTemp(Config));
// Poll from the owner; never wait for initialization on the game thread.
if (Initialization.IsReady())
{
    auto Client = Initialization.Get();
    if (Client && !Client->IsDisabled())
    {
        FPostHogProperties Properties;
        Properties.SetString(TEXT("level"), TEXT("tutorial"));
        Client->Capture(TEXT("level_started"), Properties);
        // Retain Client in the owning runtime instance.
    }
}
```

The native client also delegates registered/person/group properties, targeting overrides, automatic/manual sessions, session activity and passive session inspection. Direct owners forward environment changes with `SetAppInBackground`.

## Unreal Analytics provider

`PostHogAnalytics` adapts Unreal's `IAnalyticsProvider` to an explicitly initialized game-instance subsystem. Native callers can use `GetAnalyticsProvider()` or `FPostHogAnalyticsProvider::GetForWorld(WorldContext)`. Call `BindAnalyticsProvider()` to designate that instance for world-less configured-provider, stock Analytics Blueprint and AnalyticsMulticast paths; another live instance cannot steal the binding. Credentials and client options stay on subsystem setup, not Analytics INI/delegate routing. Unbind/shutdown releases only that instance's designation; retained adapters never retarget a replacement client.

Default event attributes are a replaceable provider-owned array, not registered SDK properties. On each adapter capture the provider merges them under that event's attributes (an event attribute wins on a key collision) and sends the result as ordinary event properties. Because they are event properties, a default attribute outranks an SDK registered, platform or generated property with the same key; Rust fills only keys that neither layer sets. Defaults do not leak into direct client captures. JSON fragments remain typed; ordinary Analytics attribute strings remain strings. BeforeSend sees the merged properties and can remove them.

`StartSession` uses the SDK session and captures `SessionStart` once per admitted SDK-session annotation; a dropped or callback-closed start returns false. `EndSession` attributes `SessionEnd` to the passively observed SDK session, including an expired session, and conditionally ends the SDK-current session without clearing a newer session created by a hook. **`SetSessionID` is unsupported and always returns false**, including for valid UUIDs; use native SDK-created automatic/manual sessions. Identity and flush calls delegate to the same client, and `Immediate` events request a flush without promising ingestion.

## Feature flags

**Get Feature Flag** returns an owned result with availability, enabled state, boolean/variant value, payload presence/value and the complete SDK record. Known false differs from missing. A present JSON null payload has `bHasPayload = true`.

Reporting can use the configured **Default**, explicit **Send**, or **Silent**. The Rust SDK owns exposure policy and deduplication; a reported read of a missing flag records `$feature_flag_called` with `$feature_flag_error = flag_missing`. Full flag getters do not record exposures and include known disabled flags.

**Reload Feature Flags** requests a refresh; it does not return a request-correlated future, and flush does not wait for flag loads. **On Feature Flags Changed** publishes the bounded mailbox's last received SDK update on the game thread, including automatic, restored and manual loads. Unread updates coalesce. Notifications include enabled flags only; use **Get Feature Flags** for all cached results. Error/partial/payload diagnostics come from the SDK. An update describes its producing evaluation, not necessarily current targeting at game-thread delivery time.

## Hooks, flush and shutdown

`FPostHogConfig::BeforeSend` receives the complete assembled `FJsonObject`, including options, groups, timestamp and UUID. Return false to drop; preserve every field you do not modify. Analytics hooks run on the capturing thread, while screenshot hooks run on Rust's encoder worker. Hooks must be thread-safe and non-throwing; do not access UObjects off the game thread. Same-client recursive capture is rejected. Use weak references for client back-references, and never wait for another capture from a hook.

`FlushAsync` reports independent state-persistence and delivery progress. A completed attempt can leave retries pending. It includes earlier submitted replay encoding and eligible delivery, not host-held acquisitions or guaranteed ingestion.

`ShutdownAsync` is one operation: it cancels renderer SDK-call admission before native shutdown begins, then retires GPU/host work and reports `bJoined` after native and host cleanup. Late readbacks are discarded. Native-hook and attached render-thread calls report `Deferred`; return without blocking the engine thread needed for retirement. Thread creation errors do not release live callback contexts. Module teardown owns the final join. Live Coding/dynamic module reload is unsupported. Release retained client/frame references and join your own caller threads before module unload.

## Screenshot replay

Recording is stopped by default. **Start Session Replay** requests manual recording; consent, valid session and supported remote permission still apply. A start made while opted out is ignored; start again after opting in. Remote replay settings load asynchronously and flush does not wait for them, so check **Is Session Replay Active** before expecting frames. **Stop Session Replay** stops new acquisitions, but valid outstanding acquisitions can still complete.

The game-instance subsystem attaches automatic capture to its own game viewport. It captures the visible axis-aligned viewport rectangle from that window's composited backbuffer, excluding unrelated windows and editor chrome outside that rectangle. Direct clients do not attach a viewport. Replay defaults are `IntervalMs = 1000` and `MaxDimension = 960` (minimum 64). `IntervalMs` is the Rust SDK's `throttle_delay`: the minimum time between replay frames for every producer of that client. **Begin Replay Frame** returns `Throttled` sooner than that after the previous frame began; skip the frame and try again on a later render frame. `0` disables the throttle for manual producers; the automatic viewport recorder never polls faster than 100 ms. Other refusals, including not recording before start or after stop, return `Inactive`.

**Automatic captures are unredacted.** They include readable UI and other content displayed inside the viewport. Enable recording only for content approved for collection. This is not a screenshot-privacy guarantee.

For manual producers, immediately before acquiring pixels, call **Begin Replay Frame** with the final width, height and row stride. Keep the returned Blueprint frame object through acquisition, then **Submit** complete top-down BGRA8888 bytes or **Abandon** it. Every submission consumes the lease, including failures; garbage collection abandons an unused lease. Native C++ provides the same single-use `FPostHogReplayFrame` reservation.

Manual pixels must already be composited and downscaled; apply any required redaction before submission. Alpha is ignored. Rust pins attribution at acquisition, validates completion, encodes JPEG, runs hooks and delivers to `/s/`. Capacity includes acquisition through retries, so skip a frame when acquisition is inactive. Replay is memory-only and can be lost on process exit or shutdown.

The automatic path uses two bounded host staging permits independently of Rust capacity. It converts/downscales off the render thread; Rust owns encoding. Window/viewport/size and foreground transitions cancel obsolete host work. See [implementation status](docs/implementation-status.md) for matching-consumer and rendered validation results and remaining platform limits.

## Tests

Use the maintained `sdk-mock-server`, not a production account:

```sh
python3 -m pip install -r Tests/requirements.txt
python3 -m unittest discover -s Tests -p 'test_*.py' -v
python3 Tests/native_integration.py \
  --library rust/target/aarch64-apple-darwin/release/libposthog_unreal_ffi.dylib \
  --mock-server /path/to/sdk-mock-server/server.py
```

After installing matching ABI-v3 artifacts and building the host, add `--editor "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"` for C++/Blueprint automation.

The current Rust pin passes 8 bridge tests, 27 tooling tests and the 19 standalone native driver tests. Its C++/Blueprint automation, rendered replay and packaged runs have not been run yet: they are pending a UE 5.8 reinstall. The previous pin passed all 13 engine tests and Metal replay in Development Editor and locally ad-hoc-signed packaged Development/Shipping. The retained example's full gameplay suite is not green. See [implementation status](docs/implementation-status.md) for known failures and remaining validation, and [CI documentation](docs/ci.md) for fixture commands and local signing steps.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) and [AI_POLICY.md](AI_POLICY.md).

[Report a bug](https://github.com/PostHog/posthog-unreal/issues/new?template=bug_report.yml) · [Community](https://posthog.com/questions) · [MIT License](LICENSE)
