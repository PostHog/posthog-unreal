# PostHog Unreal SDK

PostHog SDK for Unreal Engine, currently in development. Native delivery, the stateful C++ client, cached feature flags, game-instance ownership, Project Settings, and application lifecycle capture are implemented. Core Blueprint nodes and Unreal Analytics integration are implemented; the remaining Blueprint function/async surface is still being built. **Not ready for production use.**

See [the integration plan](docs/integration-plan.md) and [implementation status](docs/implementation-status.md).

## Building the development checkout

Current validation target: UE 5.8 on macOS arm64. Source builds require Rust, Python 3.11+, Xcode, and the upstream Rust embedding changes described in the implementation status.

```sh
export POSTHOG_RS_PATH=/path/to/posthog-rs-embedding-checkout
export PYTHON=python3
export UE_ROOT="/path/to/UE_5.8"

./scripts/build-native.sh aarch64-apple-darwin
./scripts/build-example.sh
```

The native build writes ignored artifacts and a checksum/version manifest under `Source/ThirdParty/PostHogRust/lib/`. The example build links this checkout into its ignored `Plugins` directory.

## macOS packaged games

Sandboxed macOS apps need the `com.apple.security.network.client` entitlement for PostHog delivery. UE 5.8's default Shipping entitlements omit network access even though Development permits it. With Unreal's default entitlements, select the client template in the project's `Config/DefaultEngine.ini`:

```ini
[/Script/MacTargetPlatform.XcodeProjectSettings]
ShippingSpecificMacEntitlements=(FilePath="/Game/Build/Mac/Resources/Sandbox.Client.entitlements")
```

If your project uses custom entitlements, add outgoing-network access to that file instead of replacing its other permissions.

## Playable runner example

After building, open `Examples/PostHogExample/PostHogExample.uproject` directly or run `./scripts/run-example.sh`, press **Play** in Unreal, and wait for the countdown. The three-lane runner uses only built-in engine shapes, so no character or map assets are needed. It works without analytics credentials; optional capture demonstrates run/coin events, game-instance ownership across restarts, and a cached difficulty flag.

See [the example README](Examples/PostHogExample/README.md) for controls, runtime configuration, and mock-backed gameplay tests. The current launch script targets UE 5.8 on macOS arm64, matching the development build above.

## Game-instance integration

Use **Project Settings → Plugins → PostHog** to configure automatic setup. A project token is required. Editor/PIE capture and dedicated-server initialization are disabled unless explicitly enabled. Runtime options include lifecycle capture, flag preloading, consent, person profiles, sessions, and delivery settings. Keep credentials out of source-controlled configuration.

For explicit setup, disable `bAutoInitialize` and obtain `UPostHogSubsystem` from your game instance (or Unreal's Game Instance Subsystem Blueprint node). Pass a complete `FPostHogOptions` to `Setup`; runtime setup does not inherit project credentials. Setup returns whether initialization was accepted, not whether it finished. Bind `OnSetupCompleted` before setup or poll `IsInitialized`. Both setup and shutdown completion delegates run on the game thread.

C++ code can use the initialized instance's `GetClient()` to access analytics and flags. `SetupWithConfig` additionally supports a native `BeforeSend` hook and custom context. Retained native client pointers are closed when their owning game instance shuts down.

Storage defaults to `Saved/PostHog`, with separate runtime, editor-standalone, and PIE-instance namespaces. Concurrent non-PIE game instances need distinct `ClientName` values; conflicting live owners are rejected. A replacement owner waits off-thread for cancelled initialization or shutdown of its predecessor. World changes within the same game instance retain the client.

The subsystem samples OS, engine version, locale, and app metadata on the game thread. Application version defaults to the project's Description settings; application build is optional. Lifecycle capture defaults on and emits `Application Installed`, `Application Updated`, `Application Opened`, and `Application Backgrounded` through normal consent/hook handling. Version/build markers survive identity reset. Core lifecycle and Slate activation signals are deduplicated; shutdown removes observers.

## Blueprint analytics and flags

Get **PostHog Subsystem** with **Get Game Instance Subsystem**, then wait for successful setup. The subsystem exposes:

- **Capture**: event name and optional typed properties. Its result reports queue admission, not delivery.
- **Identify**: distinct ID, optional `Set` and `Set Once` properties. Updates local identity immediately and schedules flag refresh; false also covers duplicate identifies with no changes. Switching between identified accounts requires a native reset first.
- **Get Feature Flag**: a local cache read returning `Found`, `Value`, payload presence/value, and metadata. Break the result struct and use **Try Get Boolean** or **Try Get String** on `Value`. A missing flag differs from a known false flag. `Send Event` defaults to true, so this node has execution pins.
- **Reload Feature Flags**: starts an asynchronous refresh. Bind **On Feature Flags Reloaded** before calling it; the event supplies status, persistence acknowledgement, and a completion-time flag snapshot on the game thread. Repeated calls before notification share one completion. Context changes after native completion produce `Stale` with no snapshot; shutdown or game-instance destruction cancels notification. This event covers explicit reloads, not automatic setup/identity refreshes.

Create a **PostHog Properties** variable and fill it with **Set Property**, using **Make String/Boolean/Number/Object/Array/Null Value** nodes. Alternatively, **Parse Properties** validates a JSON object and reports success. **Properties to Json** and **Value to Json** expose snapshots and payloads without losing their JSON types.

Before setup finishes or after shutdown, Capture returns `Closed`, Identify and Reload Feature Flags return false, and Get Feature Flag returns a missing result. A true reload return value means a completion was scheduled, not that the request succeeded; for example, opted-out clients complete with `Disabled`.

## Unreal Analytics integration

The `PostHogAnalytics` provider module implements Unreal's standard `IAnalyticsProvider` interface over the **existing game-instance client**. It does not create a second client, identity, delivery queue, or lifecycle owner.

### World-aware C++ callers

Add `PostHog` to your module dependencies and include `PostHogAnalyticsProvider.h`. After subsystem setup succeeds:

```cpp
auto Analytics = FPostHogAnalyticsProvider::GetForWorld(WorldContextObject);
if (Analytics)
{
    Analytics->RecordEvent(TEXT("level_started"),
        MakeAnalyticsEventAttributeArray(TEXT("level"), TEXT("tutorial"), TEXT("score"), 42));
}
```

You can also call `Subsystem->GetAnalyticsProvider()` directly. Lookup is game-thread-only and returns null until ready. Each game instance has its own stable adapter; world changes within that instance retain it. Adapter event methods operate on native data and are thread-safe.

### Unreal's built-in Analytics Blueprint nodes

Enable Unreal's **Analytics Blueprint Library** plugin and select the provider in the game's `DefaultEngine.ini`:

```ini
[Analytics]
ProviderModuleName=PostHogAnalytics
```

Initialize PostHog through Project Settings or explicit `Setup` as usual. After successful setup, call **Bind Analytics Provider** on the intended **PostHog Subsystem**. Unreal's stock Analytics nodes and `FAnalytics::Get().GetDefaultConfiguredProvider()` then use that same client.

Binding is explicit and process-wide because the stock nodes do not receive a world. A second live instance cannot replace the binding; binding the same instance again succeeds. **Unbind Analytics Provider** releases only the caller's binding, and shutdown/destruction unbinds automatically. Before binding, or after unbinding, the factory returns null. In multi-instance PIE, stock nodes target the designated instance regardless of the caller's world; use instance-specific PostHog nodes/adapters for isolation.

Configure credentials and client options on the subsystem, not in Analytics INI sections or configuration delegates. The factory rejects supplied `ProjectToken`, `Host`, or `ClientName` values instead of silently applying them to an already-bound client. Binding does not change the engine's configured provider.

For **AnalyticsMulticast**, configure `PostHogAnalytics` in `ProviderModuleNames` and bind before the multicast provider is first created. Retained adapters, including those held by multicast, stay attached to their original client. Rebinding affects new factory lookups only; old adapters close with their owner and never attach to a replacement client.

### Behavior mapping

- `RecordEvent` captures through the native client; `Immediate` requests an asynchronous flush. `FlushEvents` never waits for delivery.
- Native numeric/boolean/JSON-fragment attributes retain their JSON types. Ordinary strings stay strings, including `"00123"`. Invalid JSON fragments are logged without their contents and omitted.
- Adapter defaults apply below registered properties, SDK context, and explicit event attributes. Replacing defaults leaves registered properties intact. Defaults apply only to events recorded through that adapter.
- `SetUserID` uses `Identify`: empty IDs and switching between identified accounts are rejected. Use PostHog's native reset before switching accounts.
- `StartSession` emits `SessionStart` with its attributes once per active session; `EndSession` emits `SessionEnd` when a session exists, clears it, and requests a flush. These are ordinary events, distinct from application lifecycle events. Session IDs and the configured session mode are shared with the native client: Explicit mode allows sessionless events before start/after end; Automatic mode creates a session on subsequent foreground activity. `SetSessionID` accepts UUIDv7 IDs.
- Unreal's purchase, currency, progression, error, and demographic helpers retain their standard event mappings. `RecordError` is an ordinary event, not crash reporting.
- Consent and shutdown apply equally to all entry points. Releasing an adapter does not shut down its game instance.

## Stateful C++ client

Add `PostHog` to your module dependencies and include `PostHogClient.h`.

```cpp
FPostHogConfig Config;
Config.ProjectToken = ProjectTokenFromYourConfiguration;
Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHog");
Config.ClientName = RuntimeInstanceName; // Distinct for each concurrent PIE instance.
auto Initialization = FPostHogClient::CreateAsync(MoveTemp(Config));

// Poll readiness from the owner; never block the game thread on a future.
if (Initialization.IsReady())
{
    FPostHogClient::FPtr Client = Initialization.Get();
    if (Client)
    {
        FPostHogProperties Properties;
        Properties.SetString(TEXT("level"), TEXT("tutorial"));
        Client->Capture(TEXT("level_started"), Properties);
        // Retain Client in the owning runtime instance.
    }
}
```

The client supports identity, registered/person/group properties, consent, reset, sessions, and a C++ `BeforeSend` hook. Properties are value copies with typed JSON values. The subsystem handles lifecycle binding automatically. For a directly owned client, sample context yourself, call `StartApplicationLifecycle` once ready, and forward `OnBackground`/`OnForeground` from your owner.

`FlushAsync`, `OptOutAsync`, `OptInAsync`, `ResetAsync`, and `ShutdownAsync` return completion futures. Completion can run off-thread: marshal UObject work to the game thread. Capture acceptance is not a delivery receipt. Call `ShutdownAsync` when the owner stops; module teardown also closes clients and joins their workers. Live Coding and dynamic module reloading are not supported.

## Feature flags

Flag getters are local and never wait for a network request. Initialization preloads flags by default (`bPreloadFeatureFlags`); `ReloadFeatureFlagsAsync()` returns a status, a completion-time values snapshot, and a cache-persistence acknowledgement. Concurrent reloads share a cycle; identity/context changes queue the latest context rather than parallel requests.

```cpp
auto Flag = Client->GetFeatureFlagResult(TEXT("new-checkout"));
if (Flag.bFound)
{
    // Value is a boolean or variant string. bHasPayload distinguishes absence
    // from a valid JSON null payload; Payload retains its decoded JSON type.
}
bool Enabled = Client->IsFeatureEnabled(TEXT("new-checkout"), /*bSendEvent=*/true, /*bDefaultValue=*/false);
auto Payload = Client->GetFeatureFlagPayload(TEXT("new-checkout")); // No exposure event.
auto Refresh = Client->ReloadFeatureFlagsAsync(); // Poll readiness; do not block the game thread.
```

Reads record deduplicated `$feature_flag_called` events unless disabled per call or through `bSendFeatureFlagEvents`. Server-gated minimal exposures are filtered after `BeforeSend`. Capture attaches cached flag values and active keys.

Use `SetPersonPropertiesForFlags` / `SetGroupPropertiesForFlags` for persisted targeting overrides; corresponding reset methods clear them. These setters reload by default. Analytics person/group property updates do not automatically mirror into targeting overrides. Identity, group membership, targeting, reset, and opt-out invalidate cached context; refresh failures retain values only for the same context. Cache persistence is optional when the combined state would exceed its bounds; identity and consent take priority.

Malformed serialized payloads currently inherit Rust's raw-string fallback. This is a known pre-release spec gap; valid decoded strings are preserved without reparsing. See [implementation status](docs/implementation-status.md) for remaining flag and release work.

## Native integration tests

Use the maintained SDK mock server from the `sdk-mock-server` tooling. The test runner starts and stops an isolated server for each test. It does not use a real PostHog account.

```sh
"$PYTHON" Tests/native_integration.py \
  --library rust/target/aarch64-apple-darwin/release/libposthog_unreal_ffi.dylib \
  --mock-server /path/to/sdk-mock-server/server.py \
  --editor "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd"
```

Omit `--editor` to run only standalone native tests. Editor logs and automation reports are written under ignored `artifacts/editor-integration/`.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## Questions and feedback

- [Report a bug](https://github.com/PostHog/posthog-unreal/issues/new?template=bug_report.yml)
- [Request a feature](https://github.com/PostHog/posthog-unreal/issues/new?template=feature_request.yml)
- [Ask the PostHog community](https://posthog.com/questions)

## License

[MIT](LICENSE).
