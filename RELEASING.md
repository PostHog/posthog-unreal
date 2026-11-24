# Releasing PostHog Unreal SDK

Releases publish a single drop-in plugin archive (`posthog-unreal-<version>.zip`) plus `SHA256SUMS` to GitHub Releases. The ZIP contains the `PostHog/` plugin directory with prebuilt static libraries for macOS arm64, Linux x64, and Windows x64, along with third-party and Rust license notices.

## 1. Bump the version

Update `PostHog.uplugin`, `rust/posthog-stateful-ffi/Cargo.toml`, `rust/Cargo.lock`, and `Source/PostHog/Private/PostHogClient.cpp` together:

```sh
python3 scripts/bump-version.py 0.0.2
python3 scripts/package-plugin.py check-version
```

Commit and merge the version bump PR to `main`.

## 2. Optional dry-run candidate ZIP

Trigger the **Release** workflow manually (`workflow_dispatch`) on `main` to build all three native libraries and download the `posthog-unreal-plugin` artifact (`posthog-unreal-<version>.zip`). Extract `PostHog/` into a test Unreal 5.8 C++ project's `Plugins/` directory to verify drop-in installation before tagging.

## 3. Publish the GitHub Release

Tag the merged commit on `main` using the exact version string without a `v` prefix:

```sh
git tag 0.0.2
git push origin 0.0.2
```

Pushing a semver tag runs `.github/workflows/release.yml`, which verifies version alignment, builds and tests all three platforms (Linux x64 in an AlmaLinux 8 / glibc 2.28 container matching UE 5.8's Linux sysroot), generates third-party notices via `cargo-about`, assembles and checksums `posthog-unreal-<version>.zip`, verifies the tagged commit is on `main`, and creates the GitHub Release (`--prerelease --latest=false` automatically for `0.x` or pre-release tags).

## Pre-release governance notes

During `0.x` pre-release development, releases publish directly from tags on `main` using `GITHUB_TOKEN` and GitHub's generated release notes rather than the full production SDK release pipeline (Changesets, `CHANGELOG.md`, dedicated Releaser GitHub App, and a required-reviewer `Release` environment). Before `1.0`, configure a protected `Release` GitHub environment (with required reviewers, self-review prevention, and tag policy), add a tag protection ruleset, enable release immutability, and migrate `publish` to that environment.
