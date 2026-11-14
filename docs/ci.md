# CI and desktop validation

`Tests/Unreal/PostHogTestHost.uproject` is the authoritative asset-free SDK consumer. C++ adapter tests live in `Source/PostHog/Private/Tests`; reflected Blueprint and Analytics routing tests live in the host. The playable runner remains in `Examples/PostHogExample`, with its own Editor/gameplay checks. Build it with `scripts/build-example.sh` (managed plugin copy), then use `Tests/runner_integration.py` for its gameplay smoke. Example assets/features are independent of the asset-free SDK fixture.

## Layers

| Gate | What it establishes |
| --- | --- |
| Python tooling | Literal production include boundaries/classification with negative fixtures, mocked build/provenance commands, plugin-copy lifecycle, archive checks and synthetic JPEG assertions |
| Rust ABI / native mock tests | Exact-source public-entry behavior, buffers, hooks, close/frame ownership, real JPEG encoding and wire data |
| Candidate UHT / C++ syntax | Reflected declarations and source compilation without linking or loading native artifacts |
| UE C++ / Blueprint automation | A matching-artifact consumer, game-instance lifetime, delegates, lifecycle sequencing/option/consent/reentrant close, prepared pixels and teardown |
| Automatic rendered fixture | Owning-viewport readback, orientation/colors, unrelated-window rejection, dimensions/resize, stop and process teardown using an actual rendering backend |
| Packaged/runtime matrix | Local Mac arm64 Development/Shipping passed; wider platform/configuration and Blueprint-only consumers remain required |

The rendered fixture draws a non-sensitive four-color viewport and an unrelated magenta window. The production recorder must acquire screenshots; the fixture does not manually submit pixel buffers. The driver verifies explicit start, decoded colors/orientation, actual viewport aspect ratio, resized 16:9 frames, retained identity/session linkage, UUIDs, stable delivery after permitted outstanding work finishes on stop, and clean exit. Pixel content is unredacted. These checks do not establish privacy, live ingestion or backend playback. A missing render toolchain or an early exit without these images is a blocked/failed gate, even if the process returns zero.

## Local commands

Use Python 3.11+ (CI uses 3.13), Rust and the platform's UE 5.8 toolchain. Cargo uses an immutable SSH Git pin; configure read access to GitHub. No separate source-ref variable or local checkout is required.

```sh
python -m pip install -r Tests/requirements.txt
python -m unittest discover -s Tests -p 'test_*.py' -v
cargo test --manifest-path rust/Cargo.toml --locked

# Installs a library/manifest into the source plugin; coordinate with consumers first.
python scripts/build-native.py x86_64-unknown-linux-gnu --test
export UE_ROOT=/path/to/UE_5.8
python scripts/build-test-host.py
python Tests/native_integration.py \
  --library rust/target/x86_64-unknown-linux-gnu/release/libposthog_unreal_ffi.so \
  --mock-server /path/to/sdk-mock-server/server.py \
  --editor "$UE_ROOT/Engine/Binaries/Linux/UnrealEditor"

export VK_ICD_FILENAMES=/path/to/lvp_icd.x86_64.json
python Tests/replay_smoke.py \
  --editor "$UE_ROOT/Engine/Binaries/Linux/UnrealEditor" \
  --mock-server /path/to/sdk-mock-server/server.py --manual-start
```

Omit `--editor` for standalone native tests. The driver starts/stops isolated maintained mock servers and retains its engine-launch/control-plane support. It is the authoritative ABI v3 suite.

Windows uses `x86_64-pc-windows-msvc`, `posthog_unreal_ffi.dll` and `Engine/Binaries/Win64/UnrealEditor.exe`. The engine fixture selects WARP. Linux uses Xvfb/lavapipe and requires selecting its software ICD. macOS arm64 uses `aarch64-apple-darwin`, `libposthog_unreal_ffi.dylib` and `Engine/Binaries/Mac/UnrealEditor`; use `--renderer native` for the fixture. Mac evidence does not validate WARP/lavapipe or Linux distribution ABI compatibility.

`build-test-host.py --package Development` or `--package Shipping` provides build/cook/package commands targeting `artifacts/packages`. Its command construction is tooling-tested; the validated Mac packages used direct UAT commands as described below, not an end-to-end run of this helper. Packaging and runtime are separate gates. Managed plugin refresh replaces source/descriptor while retaining compiled products; always rebuild after changing the ABI. Do not symlink an entire SDK repository into a consumer's Plugins directory.

For packaged-runtime replay validation, select `--game-executable` instead of `--editor`. Pass the packaged binary, including the executable inside an app bundle on macOS:

```sh
python Tests/replay_smoke.py \
  --game-executable "artifacts/packages/Mac/Development-AdHoc/PostHogTestHost.app/Contents/MacOS/PostHogTestHost" \
  --renderer native --mock-server /path/to/sdk-mock-server/server.py --manual-start
```

The editor mode launches the source project; packaged mode launches its binary directly. Both use the same capture/resize/stop/resume/active-exit assertions and timeouts. `result.json` identifies `launch_mode` as `editor` or `packaged`. Successful cooking/packaging alone does not establish packaged-runtime behavior; that requires a successful run of the packaged fixture.

## Local Mac non-distribution packaging

The test host sets `bUseAutomaticCodeSigning=False` in its Mac Xcode project settings. Manual signing can still select an Apple Development identity and require a team/profile. For local checks without developer-account signing, use xcodebuild's documented `XCODE_XCCONFIG_FILE` environment override. It applies to all targets and overrides command-line/build settings; child xcodebuild processes inherit it, including UBT **PostBuildSync**. UAT-only `-NoCodeSign` / `-XcodeBuildOptions` do not by themselves control that earlier UBT substep.

Create an external temporary configuration, then preflight both game configurations from the repository root:

```sh
NO_SIGN_DIR="$(mktemp -d)"
NO_SIGN_CONFIG="$NO_SIGN_DIR/local-no-sign.xcconfig"
printf '%s\n' \
  'CODE_SIGNING_ALLOWED = NO' \
  'CODE_SIGNING_REQUIRED = NO' \
  'CODE_SIGN_IDENTITY =' \
  'CODE_SIGN_STYLE = Manual' \
  'DEVELOPMENT_TEAM =' \
  'PROVISIONING_PROFILE_SPECIFIER =' > "$NO_SIGN_CONFIG"
PROJECT="$PWD/Tests/Unreal/PostHogTestHost.uproject"

for CONFIG in Development Shipping; do
  XCODE_XCCONFIG_FILE="$NO_SIGN_CONFIG" \
    "$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" \
    PostHogTestHost Mac "$CONFIG" "$PROJECT" -architecture=arm64 \
    -WaitMutex -NoUBA -NoHotReload -NoPCH -NoSharedPCH -MaxParallelActions=2 \
    > "$NO_SIGN_DIR/preflight-$CONFIG.log" 2>&1 || exit 1
done
```

Before packaging, inspect **both** preflight logs. The actual PostBuildSync invocation must omit `-allowProvisioningUpdates` and authentication-key flags; effective settings must show signing allowed/required as `NO`, manual style and empty identity/team/profile. Stop if they do not. The recorded local Development and Shipping preflights passed this check before UAT packaging.

UE 5.8's Mac toolchain requires `Contents/_CodeSignature/CodeResources` as a staging receipt resource even with build signing disabled. A fresh wholly unsigned package therefore does not pass staging. The validated local workflow explicitly ad-hoc seals each built source app before UAT, then seals the archived app again after package rewriting and verifies its final signature. This supplies real signature metadata without a developer certificate or provisioning profile.

After both preflights, use the same per-process override with direct UAT and local ad-hoc signing:

```sh
for CONFIG in Development Shipping; do
  case "$CONFIG" in
    Development) APP_NAME=PostHogTestHost.app ;;
    Shipping) APP_NAME=PostHogTestHost-Mac-Shipping.app ;;
  esac
  SOURCE_APP="$PWD/Tests/Unreal/Binaries/Mac/$APP_NAME"
  ARCHIVE_DIR="$PWD/artifacts/packages/Mac/$CONFIG-AdHoc"
  /usr/bin/codesign --force --sign - "$SOURCE_APP" || exit 1
  /usr/bin/codesign --verify --strict "$SOURCE_APP" || exit 1
  XCODE_XCCONFIG_FILE="$NO_SIGN_CONFIG" \
    "$UE_ROOT/Engine/Build/BatchFiles/RunUAT.sh" BuildCookRun \
    "-project=$PROJECT" -platform=Mac -target=PostHogTestHost \
    "-clientconfig=$CONFIG" -specifiedarchitecture=arm64 \
    -build -cook -map=/Engine/Maps/Entry -stage -pak -package -archive \
    "-archivedirectory=$ARCHIVE_DIR" \
    -unattended -utf8output -nop4 -nocompileeditor -nodebuginfo -NoCodeSign \
    '-ubtargs=-NoUBA -WaitMutex -NoPCH -NoSharedPCH -MaxParallelActions=2 -SkipDeploy' \
    '-XcodeBuildOptions=CODE_SIGNING_ALLOWED=NO CODE_SIGNING_REQUIRED=NO CODE_SIGN_IDENTITY= CODE_SIGN_STYLE=Manual PROVISIONING_PROFILE_SPECIFIER=' \
    || exit 1
  ARCHIVE_APP="$ARCHIVE_DIR/$APP_NAME"
  /usr/bin/codesign --force --sign - "$ARCHIVE_APP" || exit 1
  /usr/bin/codesign --verify --deep --strict "$ARCHIVE_APP" || exit 1
  /usr/bin/codesign --display --verbose=4 "$ARCHIVE_APP" || exit 1
done
```

The validated archives are under `artifacts/packages/Mac/Development-AdHoc` and `Shipping-AdHoc`. Signature inspection reports `Signature=adhoc` and `TeamIdentifier=not set`. Keep the override scoped to these commands; UBT/UAT/Xcode remain no-sign/manual, without global Xcode or account changes. Ad-hoc sealing is local test signing, not developer-identity/provisioning, notarization or distribution validation. Run the packaged replay fixture separately for each executable after final signature verification. The Shipping bundle uses `PostHogTestHost-Mac-Shipping.app/Contents/MacOS/PostHogTestHost-Mac-Shipping` under its archive directory.

## GitHub Actions

`.github/workflows/sdk-tests.yml` runs tooling on pull requests/main. Protected native jobs run only on trusted pushes/manual dispatch, with the immutable dependency selected by Cargo. Engine-backed tests require the dispatch's `unreal` input. A skipped native/engine job means validation pending.

Create a protected `sdk-ci` environment with required reviewers and trusted-branch restrictions. Configure repository variables:

| Variable | Purpose |
| --- | --- |
| `SDK_MOCK_ARCHIVE_SHA256` | SHA-256 of maintained mock ZIP; enables the native matrix |
| `SDK_GIT_KNOWN_HOSTS` | Verified GitHub SSH known-hosts entry/entries; strict host checking is enabled |
| `UE_WINDOWS_ARCHIVE_SHA256` | Authorized Windows UE 5.8 installed-build ZIP checksum |
| `UE_LINUX_ARCHIVE_SHA256` | Authorized Linux UE 5.8 installed-build ZIP checksum |

Configure protected environment secrets:

| Secret | Purpose |
| --- | --- |
| `SDK_GIT_SSH_KEY` | Read-only GitHub SSH key for Cargo's pinned Rust dependency |
| `SDK_MOCK_ARCHIVE_URL` | Private download URL for the maintained mock ZIP |
| `UE_WINDOWS_ARCHIVE_URL` | Authorized private engine ZIP URL |
| `UE_LINUX_ARCHIVE_URL` | Authorized private engine ZIP URL |

The SSH key and verified host entries are written only to the ephemeral runner's private temporary files; they are not uploaded. Engine/mock assets and their credentials are not exposed to pull-request code.

The mock archive must have `server.py` and required companion files at its root, with v1 analytics, snapshot dumps and the control/inspection endpoints used by tests. Engine archives must contain `Engine/` with headers, libraries, UBT/UAT, editor binaries and bundled toolchains. Obtain them through authorized Epic access and keep them protected. The workflow verifies checksums and redacts download errors; it never uploads engine files.

Check extraction size and runner free space before enabling engine jobs. Platform provisioning is separate from SDK correctness. Do not infer supported runtimes from build-script triple names or mocked command tests.

## Evidence

The current `b717aef23d4869bdf94e75ce36bbd383d3575c6f` integration passes 7 bridge tests, 27 tooling tests and the 20-test consolidated driver with matching installed release artifacts on UE 5.8 Mac arm64. All 13 engine tests pass, including real automatic-session expiry attribution, newer reentrant session preservation and Analytics defaults/stock Blueprint/Multicast routing. Both asset-free and retained example Editor consumers build; the example's Analytics automation passes.

The real Metal fixture passes in Development Editor and locally ad-hoc-signed packaged Development/Shipping: three initial 320×240 and three resized 320×180 JPEGs per configuration, explicit-start gating, halt/stable/resume and exit code 0 with recording active. Independent verification decoded all 18 JPEGs and checked raw v1 event attribution and analytics/replay session, window and identity linkage. In raw analytics events, `session_id` and `distinct_id` are top-level fields; event library properties are `posthog-unreal/0.0.1`, distinct from the `posthog-rs/0.31.3` transport header. Packaged runs cover monolithic shutdown, including retirement of registry-held initialization futures while engine services are live.

The retained example's offline/normal/hard gameplay smoke each passes 34 checks, but the full gameplay gate is not green: delayed cached-flag expectations fail on both attempts, and the timeout case fails its low-obstacle jump after flag/input checks. No pristine-base runtime comparison establishes these as pre-existing. The pinned SDK PR #360 CI remains red on unchanged replay lint/deadline cases. Standalone release bridge validation initially hit the timing-sensitive hook-close `Accepted` versus `Closed` assertion; the unchanged rerun and ten repeats pass. See [implementation status](implementation-status.md) for these limits and the completed main-aware review/expiry recheck.

Windows/Linux, protected native/engine CI, Blueprint-only packaged consumers, embedded/multiple-PIE stress, additional HDR/backends, sustained performance, live ingestion and distribution remain unverified.

Editor and packaged Development logs supplement the protocol/image/exit evidence. Shipping produced no engine log, so its result does not establish a log-wide absence of ensures. Automatic pixels are unredacted; these gates do not establish screenshot privacy, real ingestion or player playback.

Local runtime evidence belongs under ignored `artifacts/editor-integration` and `artifacts/replay-smoke`; CI uploads those outputs and native provenance manifests, not engines or secrets. Record exact dependency/header/library hashes and toolchains alongside test results. Synthetic and syntax-only results remain distinct from matching-artifact engine runtime, automatic rendering, packaged consumers and performance evidence.
