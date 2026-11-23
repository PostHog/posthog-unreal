# Implementation status

The plugin is in development and is not production-ready. Source targets UE 5.8 on macOS arm64, Windows x64 and Linux x64; configured targets are not a declaration of completed platform validation.

## Implemented source

- Immutable stateful Rust dependency: package `posthog`, revision `9cad3ee2a8a9313fbda2d9c0ad80db9e3e8ce812` (package version 1.3.0, head of the reviewed stateful stack on `feat/stateful-session-replay`), `tls` and `session-replay`, default features disabled.
- ABI v3 marshaling, owned outputs, complete before-send callbacks, flag mailbox, single-use screenshot acquisitions and tracked shutdown.
- Thin C++ delegation for identity, consent, registered/person/group properties, sessions and flags.
- Native lifetime registry, callback-origin deferred close, host-call accounting and final module teardown, including joined initialization-owner retirement before engine event-pool teardown.
- Game-instance setup/cancellation, namespace reuse, sampled engine context, environment/PIE isolation and game-thread flag notifications.
- Default-on host lifecycle capture: Application Opened (`from_background=false` at foreground setup, `true` on return) and Application Backgrounded; deduplicated engine signals continue to drive sessions/replay when analytics are disabled. Application Installed and Application Updated are not captured: Unreal has no reliable install or update signal, so this differs from the sdk-specs application-lifecycle spec and the iOS and Android SDKs on purpose.
- Blueprint typed values, analytics/identity/consent, flag reads/reload, manual replay start/stop and owned prepared-pixel frame objects.
- Automatic owning-viewport source: same-paint window association/cropping, two host staging permits, GPU readbacks, worker conversion/downscale and joined retirement; Rust still owns JPEG/envelope/delivery and manual stopped-by-default recording.
- Instance-bound Unreal Analytics provider/factory, stock Analytics Blueprint and Multicast routing, adapter-merged default attributes (sent as event properties under each event's attributes) and SDK-session annotations. Custom Analytics session IDs are unsupported.
- Consolidated native test driver, Unreal adapter/Blueprint/mechanism tests, asset-free automatic rendered fixture and provenance tooling.

## Current integration evidence

The pin moved from `b717aef23d4869bdf94e75ce36bbd383d3575c6f` to `9cad3ee2a8a9313fbda2d9c0ad80db9e3e8ce812`. At the new pin, eight Rust bridge tests, twenty-eight tooling tests and the twenty standalone tests of the consolidated driver pass on macOS arm64 against the SDK mock server, with formatting and bridge Clippy clean. The C++ adapter changes for the new pin (Analytics default merge, `Throttled`, interval-as-throttle and asynchronous settings/flag waits in tests) have **not** been compiled or run: UHT, C++ compilation, the thirteen engine tests, the rendered fixture, packaged runs and the retained example all need a rerun on UE 5.8.

Changes adopted from the new pin:

- `fallback_properties` was removed from the SDK and the ABI. The Analytics provider merges its default attributes under each event's attributes and sends them as event properties, so a default now outranks a registered/platform/SDK property with the same key (previously the reverse).
- Replay frames are throttled by `ReplaySettings.IntervalMs` (the SDK `throttle_delay`, default 1 s); `PHU_THROTTLED`/`EPostHogResult::Throttled` reports a skipped frame. A replay start while opted out is ignored. Remote replay settings and flag loads are asynchronous; flush does not wait for them.
- Reported reads of missing flags record `$feature_flag_called` with `flag_missing`. Flag results no longer carry `evaluation_revision`; the state read no longer reports `identified`.
- A full queue drops its oldest event instead of rejecting the capture. The transport `posthog-sdk-info` header names the configured SDK (`posthog-unreal/0.0.1`) rather than `posthog-rs`.

The remainder of this section records evidence at the previous pin, `b717aef23d4869bdf94e75ce36bbd383d3575c6f`.

The engine tests cover C++/Blueprint operations, callback close, game-instance ownership/replacement, large-property round trips, prepared-pixel replay, bounded staging, readback-allocation reuse, host retirement and lifecycle capture. Analytics coverage includes defaults/precedence, stock Blueprint/Multicast routing, callback lifetime and preservation of newer reentrant sessions. A real persistent automatic-session fixture restores an SDK-created ID before expiry, waits past its age boundary and verifies SessionEnd attributes the expired ID while safely cleaning up the SDK-current session. Pending/replacement coordination and retirement of joined initialization-owner holders are also covered.

The real Metal fixture on Apple M4 Pro passes in three UE 5.8 macOS arm64 consumer configurations: Development Editor, locally cooked/packaged Development, and locally cooked/packaged Shipping. Each previous-pin run decodes three initial 320×240 JPEGs and three resized 320×180 JPEGs, verifies quadrant colors/orientation and unrelated-window exclusion, explicit-start gating, analytics/replay identity/session linkage, UUID uniqueness, halt/stable outstanding delivery/resume, and clean exit with recording active (exit code 0). Independent verification decoded all eighteen JPEGs and checked raw canonical v1 attribution plus analytics/replay session, window and identity linkage. Raw analytics `session_id` and `distinct_id` are top-level event fields; event properties identify `posthog-unreal/0.0.1`, while the transport header identifies `posthog-rs/0.31.3`. The packaged runs exercise monolithic module teardown and explicit retirement of registry-held initialization futures after tracked initialization/native/host work joins.

Editor and packaged Development engine/stdout logs are available for inspection. Shipping produced no engine log; its evidence is the decoded images, control protocol and exit result, not a claim that no ensure was logged. These are local non-distribution packages: UBT/UAT/Xcode use a per-process no-sign/manual override, and both source and archived apps are explicitly ad-hoc sealed with `codesign --sign -`. Final archived apps pass strict/deep signature verification. This is not a wholly unsigned package, developer-identity/provisioning, notarization or distribution validation. See [CI commands](ci.md) for the PostBuildSync and staging/signature boundary. The three configurations share one Mac/backend; they are not a platform or backend matrix.

The lifecycle test completes all nine cases within its 60-second deadline: buffered startup, duplicate notifications, the reflected host option, consent, hook drops and reentrant owner shutdown/destruction. Wire assertions verify event sequences and SDK-owned session attribution. The automatic Metal regression also passes with lifecycle capture enabled.

Tests use the maintained SDK mock server. Automatic pixels are unredacted. These results do not establish live ingestion, backend playback or screenshot privacy.

## Retained example and known failures

The retained example's actual Editor link and Analytics automation pass, including defaults, stock Blueprint/Multicast, expired-session attribution and lifetime cases. Offline, normal and hard gameplay smoke each pass 34 checks. The full legacy gameplay gate is **not green**: the delayed case fails its first-run cached-flag expectation on both attempts, and the timeout case fails a low-obstacle jump after passing its flag/input checks. No pristine-base runtime comparison was run, so these are not established as pre-existing failures. These gameplay failures remain unresolved.

At the previous pin, the Rust SDK's PR #360 CI was red on replay-policy lint and `analytics_deadline_applies_while_encoder_is_blocked` deadline/order cases. At the new pin `9cad3ee2`, PR #360 reports 59 passing checks and 1 skipped. At the previous pin, the standalone bridge release run also initially hit a timing-sensitive hook-close `Accepted` versus `Closed` assertion; an unchanged full rerun and ten targeted repeats pass. Passing reruns do not establish that the hook-close timing issue is fixed.

The full main-aware independent review and focused expired-session recheck completed without remaining findings. Release/platform review remains separate.

## Required next milestones

1. Rerun UHT/C++ compilation, the engine automation, the rendered replay fixture, packaged Development/Shipping and the retained example at the new pin on UE 5.8.
2. Stress embedded/multiple-PIE surface isolation, foreground/window/screen transitions, resize races and GPU/worker teardown.
3. Validate actual HDR/backend formats, resource bounds and sustained renderer operation beyond the current Metal fixture.
4. Validate Windows/Linux engine and packaged runtime, protected native/engine CI, Blueprint-only packaged consumers and the wider desktop configuration matrix.
5. Validate storage failures, offline/retry/resource behavior and realistic capture/frame-time/shutdown budgets across the supported matrix.
6. Complete release/platform review and artifact/license/toolchain evidence across the intended release matrix.

Full Blueprint convenience/async coverage remains separate development work. No release readiness is implied by source completion or tooling-only CI.

See [architecture](integration-plan.md), [core boundary](stateful-rust-core-plan.md) and [CI commands](ci.md).
