---
'posthog-unreal': minor
---

First experimental prerelease of the PostHog Unreal SDK for UE 5.8 C++ projects: a thin Unreal adapter over the pinned stateful posthog-rs core, which owns identity, properties, consent, sessions, feature flags, remote configuration, persistence and delivery. It adds Blueprint and C++ APIs, an Unreal Analytics provider, application lifecycle events, and viewport and manual screenshot replay (automatic captures are unredacted). Install the `posthog-unreal-<version>.zip` release asset, which bundles prebuilt Mac arm64, Linux x64 and Windows x64 Rust static libraries, checksums and third-party notices; no Rust toolchain is needed. Not production-ready; see the README status for platform validation.
