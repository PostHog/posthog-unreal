# PostHog Unreal SDK

PostHog SDK for Unreal Engine, currently in pre-release (`0.x`, **not ready for production use**; the C++, Blueprint, and C FFI API surface is subject to breaking changes before `1.0`).

Unreal acts as a thin host adapter over the stateful Rust SDK (`posthog::stateful::Client`), bridged through a C FFI crate (`rust/posthog-stateful-ffi`) modeled on `posthog-rs-ffi` so it can be migrated into `posthog-rs`. Persisted client state is stored under `Saved/PostHogStateful` by default (namespaced per runtime/editor/PIE instance).

- **Rust owns**: identity, consent, registered/person/group properties, sessions, feature flags, remote config, disk persistence, event assembly, retry/delivery, and session replay policy/JPEG encoding/delivery.
- **Unreal owns**: `UPostHogSubsystem` game-instance lifecycle, Blueprint nodes, `PostHogAnalytics` (`IAnalyticsProvider`), engine platform context, lifecycle delegates (`Application Opened` / `Application Backgrounded`), and owning-viewport GPU readback.

## Installing (Drop-in Release ZIP)

Download `posthog-unreal-<version>.zip` from [GitHub Releases](https://github.com/PostHog/posthog-unreal/releases) (not the automatic source archive), extract the `PostHog/` folder into `<YourProject>/Plugins/PostHog/`, and rebuild your C++ project. Prebuilt Mac arm64, Linux x64, and Windows x64 static libraries are bundled — no Rust toolchain is required.

## Building from source

Requires Rust, Python 3.11+, and Unreal Engine 5.8 for editor/example builds.

```sh
# Build and test the Rust stateful FFI and install the static library into the plugin
python3 scripts/build-native.py --test

# Build the playable example project (macOS arm64)
export UE_ROOT="/Users/Shared/Epic Games/UE_5.8"
./scripts/build-example.sh
```

The native build writes the target static library under `Source/ThirdParty/PostHogRust/lib/<target>/`. Supported desktop targets in `PostHogRust.Build.cs`: `aarch64-apple-darwin`, `x86_64-pc-windows-msvc`, and `x86_64-unknown-linux-gnu`.

## Testing

### Rust FFI tests (also run in CI)

```sh
cargo fmt --manifest-path rust/Cargo.toml --check
cargo clippy --manifest-path rust/Cargo.toml --all-targets --locked -- -D warnings
cargo test --manifest-path rust/Cargo.toml --locked
```

Integration tests in `rust/posthog-stateful-ffi/tests/ffi.rs` use `httpmock` directly—no external mock server download or secrets are required.

### Unreal automation tests and example gameplay smoke (local UE 5.8)

```sh
# Run PostHog.* automation tests headlessly in Unreal Editor
"$UE_ROOT/Engine/Binaries/Mac/UnrealEditor-Cmd" \
  "$PWD/Examples/PostHogExample/PostHogExample.uproject" \
  -ExecCmds="Automation RunTests PostHog.; Quit" \
  -unattended -nop4 -nosplash -nullrhi -stdout

# Run the playable runner example smoke scenarios against Tests/mock_server.py
python3 Tests/runner_integration.py --ue-root "$UE_ROOT"
```

To run the remote and warm-cache feature-flag/replay automation tests without skipping, start `PORT=18080 python3 Tests/mock_server.py` and export `POSTHOG_TEST_HOST=http://127.0.0.1:18080` when invoking `UnrealEditor-Cmd`.

## Architecture

### Rust FFI (`rust/posthog-stateful-ffi`)

Follows `posthog-rs-ffi` conventions:
- Opaque handles (`PostHogStatefulConfig`, `PostHogStatefulClient`, `PostHogReplayFrame`) with `*_new` / `*_free`.
- Typed C enums and structs (`PostHogStatus`, `PostHogCaptureOutcome`, `PostHogDrainReport`, `PostHogFrameUnavailable`, `PostHogSubmitOutcome`) plus `posthog_last_error()` and `posthog_string_free()`.
- Direct typed functions for every stateful operation, `cbindgen.toml`, and `Source/ThirdParty/PostHogRust/include/posthog_stateful.h`.

### Game-instance integration (`UPostHogSubsystem`)

Configure automatic setup under **Project Settings → Plugins → PostHog**, or disable `bAutoInitialize` and call `Setup` / `SetupWithConfig` explicitly on `UPostHogSubsystem`.

- Storage defaults to `Saved/PostHogStateful` with runtime/editor/PIE namespacing.
- `OnSetupCompleted`, `OnShutdownCompleted`, and `OnFeatureFlagsChanged` dispatch on the game thread.
- Lifecycle capture defaults on and emits `Application Opened` and `Application Backgrounded` from Unreal's application/Slate delegates.
- Session replay requires explicit `StartSessionReplay()` (and server-side remote config permission) and captures unredacted pixels from the owning game viewport.

### Stateful C++ client (`FPostHogClient`)

```cpp
FPostHogConfig Config;
Config.ProjectToken = ProjectToken;
Config.StorageRoot = FPaths::ProjectSavedDir() / TEXT("PostHogStateful");
auto Init = FPostHogClient::CreateAsync(MoveTemp(Config));
// Wait/poll off the game thread, then retain FPostHogClient::FPtr:
if (auto Client = Init.Get())
{
    FPostHogProperties Properties;
    Properties.SetString(TEXT("level"), TEXT("tutorial"));
    Client->Capture(TEXT("level_started"), Properties);
}
```

### Unreal Analytics (`PostHogAnalytics`)

Enable the `AnalyticsBlueprintLibrary` plugin and set:

```ini
[Analytics]
ProviderModuleName=PostHogAnalytics
```

Call `BindAnalyticsProvider()` on `UPostHogSubsystem` after setup completes so Unreal's stock Analytics Blueprint nodes and `FAnalytics::Get().GetDefaultConfiguredProvider()` route through the subsystem's client. `SetSessionID` returns `false` because sessions are managed by the Rust SDK.

## macOS packaged games

Sandboxed macOS apps require the `com.apple.security.network.client` entitlement. Select the client template in `Config/DefaultEngine.ini` when using UE 5.8's default Shipping entitlements:

```ini
[/Script/MacTargetPlatform.XcodeProjectSettings]
ShippingSpecificMacEntitlements=(FilePath="/Game/Build/Mac/Resources/Sandbox.Client.entitlements")
```

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE).
