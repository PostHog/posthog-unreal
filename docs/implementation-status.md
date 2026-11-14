# Implementation status

The plugin is in development and is not production-ready. Source targets UE 5.8 on macOS arm64, Windows x64 and Linux x64; configured targets are not a declaration of completed platform validation.

## Implemented source

- Immutable stateful Rust dependency: package `posthog`, revision `b717aef23d4869bdf94e75ce36bbd383d3575c6f`, `tls` and `session-replay`, default features disabled.
- ABI v3 marshaling, owned outputs, complete before-send callbacks, flag mailbox, single-use screenshot acquisitions and tracked shutdown.
- Thin C++ delegation for identity, consent, registered/person/group properties, sessions and flags.
- Native lifetime registry, callback-origin deferred close, host-call accounting and final module teardown, including joined initialization-owner retirement before engine event-pool teardown.
- Game-instance setup/cancellation, namespace reuse, sampled engine context, environment/PIE isolation and game-thread flag notifications.
- Default-on host lifecycle capture: Application Opened (`from_background=false` at foreground setup, `true` on return) and Application Backgrounded; deduplicated engine signals continue to drive sessions/replay when analytics are disabled.
- Blueprint typed values, analytics/identity/consent, flag reads/reload, manual replay start/stop and owned prepared-pixel frame objects.
- Automatic owning-viewport source: same-paint window association/cropping, two host staging permits, GPU readbacks, worker conversion/downscale and joined retirement; Rust still owns JPEG/envelope/delivery and manual stopped-by-default recording.
- Instance-bound Unreal Analytics provider/factory, stock Analytics Blueprint and Multicast routing, SDK-merged per-capture fallback defaults and SDK-session annotations. Custom Analytics session IDs are unsupported.
- Consolidated native test driver, Unreal adapter/Blueprint/mechanism tests, asset-free automatic rendered fixture and provenance tooling.

## Current integration evidence

The Analytics module and retained example callers use Rust revision `b717aef23d4869bdf94e75ce36bbd383d3575c6f`, including its public per-capture fallback-properties API. Matching ABI v3 release artifacts, header and manifest have been installed and linked into the UE 5.8 macOS arm64 consumers. Seven Rust bridge tests, twenty-seven tooling tests and the twenty-test consolidated driver pass, including all thirteen in-engine automation tests. Formatting, bridge Clippy and both consumers' UHT/syntax checks pass (thirty-three authored and eighteen generated translation units). Known test/CI failures are recorded separately below.

The engine tests cover C++/Blueprint operations, callback close, game-instance ownership/replacement, large-property round trips, prepared-pixel replay, bounded staging, readback-allocation reuse, host retirement and lifecycle capture. Analytics coverage includes defaults/precedence, stock Blueprint/Multicast routing, callback lifetime and preservation of newer reentrant sessions. A real persistent automatic-session fixture restores an SDK-created ID before expiry, waits past its age boundary and verifies SessionEnd attributes the expired ID while safely cleaning up the SDK-current session. Pending/replacement coordination and retirement of joined initialization-owner holders are also covered.

The real Metal fixture on Apple M4 Pro passes in three UE 5.8 macOS arm64 consumer configurations: Development Editor, locally cooked/packaged Development, and locally cooked/packaged Shipping. Each current-pin run decodes three initial 320×240 JPEGs and three resized 320×180 JPEGs, verifies quadrant colors/orientation and unrelated-window exclusion, explicit-start gating, analytics/replay identity/session linkage, UUID uniqueness, halt/stable outstanding delivery/resume, and clean exit with recording active (exit code 0). Independent verification decoded all eighteen JPEGs and checked raw canonical v1 attribution plus analytics/replay session, window and identity linkage. Raw analytics `session_id` and `distinct_id` are top-level event fields; event properties identify `posthog-unreal/0.0.1`, while the transport header identifies `posthog-rs/0.31.3`. The packaged runs exercise monolithic module teardown and explicit retirement of registry-held initialization futures after tracked initialization/native/host work joins.

Editor and packaged Development engine/stdout logs are available for inspection. Shipping produced no engine log; its evidence is the decoded images, control protocol and exit result, not a claim that no ensure was logged. These are local non-distribution packages: UBT/UAT/Xcode use a per-process no-sign/manual override, and both source and archived apps are explicitly ad-hoc sealed with `codesign --sign -`. Final archived apps pass strict/deep signature verification. This is not a wholly unsigned package, developer-identity/provisioning, notarization or distribution validation. See [CI commands](ci.md) for the PostBuildSync and staging/signature boundary. The three configurations share one Mac/backend; they are not a platform or backend matrix.

The lifecycle test completes all nine cases within its 60-second deadline: buffered startup, duplicate notifications, the reflected host option, consent, hook drops and reentrant owner shutdown/destruction. Wire assertions verify event sequences and SDK-owned session attribution. The automatic Metal regression also passes with lifecycle capture enabled.

Tests use the maintained SDK mock server. Automatic pixels are unredacted. These results do not establish live ingestion, backend playback or screenshot privacy.

## Retained example and known failures

The retained example's actual Editor link and Analytics automation pass, including defaults, stock Blueprint/Multicast, expired-session attribution and lifetime cases. Offline, normal and hard gameplay smoke each pass 34 checks. The full legacy gameplay gate is **not green**: the delayed case fails its first-run cached-flag expectation on both attempts, and the timeout case fails a low-obstacle jump after passing its flag/input checks. No pristine-base runtime comparison was run, so these are not established as pre-existing failures. These gameplay failures remain unresolved.

The pinned Rust SDK's PR #360 CI remains red on unchanged replay-policy lint and `analytics_deadline_applies_while_encoder_is_blocked` deadline/order cases. The standalone bridge release run also initially hit a timing-sensitive hook-close `Accepted` versus `Closed` assertion; an unchanged full rerun and ten targeted repeats pass. Passing reruns do not establish that the hook-close timing issue is fixed.

The full main-aware independent review and focused expired-session recheck completed without remaining findings. Release/platform review remains separate.

## Required next milestones

1. Stress embedded/multiple-PIE surface isolation, foreground/window/screen transitions, resize races and GPU/worker teardown.
2. Validate actual HDR/backend formats, resource bounds and sustained renderer operation beyond the current Metal fixture.
3. Validate Windows/Linux engine and packaged runtime, protected native/engine CI, Blueprint-only packaged consumers and the wider desktop configuration matrix.
4. Validate storage failures, offline/retry/resource behavior and realistic capture/frame-time/shutdown budgets across the supported matrix.
5. Complete release/platform review and artifact/license/toolchain evidence across the intended release matrix.

Full Blueprint convenience/async coverage remains separate development work. No release readiness is implied by source completion or tooling-only CI.

See [architecture](integration-plan.md), [core boundary](stateful-rust-core-plan.md) and [CI commands](ci.md).
