# PostHog Unreal SDK

PostHog SDK for Unreal Engine, currently in development. Native delivery, the stateful C++ client, cached feature flags, game-instance ownership, Project Settings, and application lifecycle capture are implemented. The full Blueprint function/async surface and Unreal Analytics adapter are still being built. **Not ready for production use.**

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

## Playable runner example

After building, open `Examples/PostHogExample/PostHogExample.uproject` directly or run `./scripts/run-example.sh`, press **Play** in Unreal, and wait for the countdown. The three-lane runner uses only built-in engine shapes, so no character or map assets are needed. It works without analytics credentials; optional capture demonstrates run/coin events, game-instance ownership across restarts, and a cached difficulty flag.

See [the example README](Examples/PostHogExample/README.md) for controls, runtime configuration, and mock-backed gameplay tests. The current launch script targets UE 5.8 on macOS arm64, matching the development build above.

## Game-instance integration

Use **Project Settings → Plugins → PostHog** to configure automatic setup. A project token is required. Editor/PIE capture and dedicated-server initialization are disabled unless explicitly enabled. Runtime options include lifecycle capture, flag preloading, consent, person profiles, sessions, and delivery settings. Keep credentials out of source-controlled configuration.

For explicit setup, disable `bAutoInitialize` and obtain `UPostHogSubsystem` from your game instance (or Unreal's Game Instance Subsystem Blueprint node). Pass a complete `FPostHogOptions` to `Setup`; runtime setup does not inherit project credentials. Setup returns whether initialization was accepted, not whether it finished. Bind `OnSetupCompleted` before setup or poll `IsInitialized`. Both setup and shutdown completion delegates run on the game thread.

C++ code can use the initialized instance's `GetClient()` to access analytics and flags. `SetupWithConfig` additionally supports a native `BeforeSend` hook and custom context. Retained native client pointers are closed when their owning game instance shuts down.

Storage defaults to `Saved/PostHog`, with separate runtime, editor-standalone, and PIE-instance namespaces. Concurrent non-PIE game instances need distinct `ClientName` values; conflicting live owners are rejected. A replacement owner waits off-thread for cancelled initialization or shutdown of its predecessor. World changes within the same game instance retain the client.

The subsystem samples OS, engine version, locale, and app metadata on the game thread. Application version defaults to the project's Description settings; application build is optional. Lifecycle capture defaults on and emits `Application Installed`, `Application Updated`, `Application Opened`, and `Application Backgrounded` through normal consent/hook handling. Version/build markers survive identity reset. Core lifecycle and Slate activation signals are deduplicated; shutdown removes observers.

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

Use `SetPersonPropertiesForFlags` / `SetGroupPropertiesForFlags` for persisted targeting overrides; corresponding reset methods clear them. These setters reload by default. Analytics person/group property updates do not automatically mirror into targeting overrides. Identity, group membership, and targeting changes mark the cached context stale and reload: previously loaded values stay readable in memory until the new response applies, but are not persisted under the new context. Reset and opt-out clear the cache. A failed refresh leaves the in-memory values unchanged. Cache persistence is optional when the combined state would exceed its bounds; identity and consent take priority.

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
