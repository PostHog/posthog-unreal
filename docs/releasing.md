# Releasing

The entire PostHog Unreal SDK has one version, tag and GitHub Release. The release
artifact is one installable plugin ZIP: Unreal C++ source plus matching prebuilt
Rust static libraries for all three desktop targets.

| Target | Native build runner | Bundled library |
| --- | --- | --- |
| Mac arm64 | macOS 15, deployment target macOS 14 | `libposthog_unreal_ffi.a` |
| Linux x64 | Ubuntu 22.04 | `libposthog_unreal_ffi.a` |
| Windows x64 | Windows Server 2022 / MSVC | `posthog_unreal_ffi.lib` |

The adapter targets UE 5.8. Consumers compile the C++ with Unreal, not Cargo.
Unreal links only the library for the selected platform.

## Changesets and versioning

Install the pinned release tool:

```sh
cargo install sampo --version 0.17.4 --locked
```

Sampo tracks the SDK through the internal `cargo/posthog-unreal-ffi` identifier.
Use that identifier for **all SDK changes**, including C++, Blueprint APIs and
packaging. It does not represent a separate Rust release; the crate remains
`publish = false`.

Add a Markdown file in `rust/.sampo/changesets/` for each user-visible change:

```md
---
cargo/posthog-unreal-ffi: patch
---

Describe the change for SDK users.
```

Use patch for fixes, minor for features and major for breaking changes.
You can also run `sampo add` from `rust/`.

## Prepare a release

Start from current `main` on a release-preparation branch, with a clean working
tree. Use Python 3.11+.

```sh
python scripts/release.py prepare
python scripts/release.py check
python -m pip install -r Tests/requirements.txt
python -m unittest discover -s Tests -p 'test_*.py' -v
```

Preparation runs `sampo release` in `rust/`, consumes changesets, updates the
manifest/lockfile and writes the SDK changelog at
`rust/posthog-unreal-ffi/CHANGELOG.md`. It syncs `PostHog.uplugin`'s
`VersionName` and increments its integer `Version`. The Rust bridge's SDK event
version and native provenance use the same version. The native ABI is independent.

Sampo regenerates `Cargo.lock`, which can update transitive dependencies within
the manifest constraints. Review dependency changes, generated license notices,
version and changelog in the preparation PR. Validate the resulting dependency
graph before merging. Do not run `sampo publish`.

### Build and stage native bundles

Run these commands on each native host using the same prepared release commit
and the corresponding target triple. The example uses Bash, including Git Bash
on Windows:

```sh
TARGET=aarch64-apple-darwin
# Linux: TARGET=x86_64-unknown-linux-gnu
# Windows: TARGET=x86_64-pc-windows-msvc
export RUSTUP_TOOLCHAIN=1.96.1
rustup toolchain install 1.96.1 --profile minimal --component rust-docs --target "$TARGET"
python scripts/build-native.py "$TARGET" --test
cargo install cargo-about --version 0.9.3 --locked --features cli
mkdir -p artifacts/notices
cargo about generate --manifest-path rust/Cargo.toml --locked --fail \
  --target "$TARGET" scripts/third-party-notices.hbs \
  --output-file artifacts/notices/THIRD_PARTY_NOTICES.txt
python scripts/package-plugin.py stage "$TARGET" \
  --notices artifacts/notices/THIRD_PARTY_NOTICES.txt
```

Staging checks the native
version, target, ABI and library/lockfile hashes and writes to a fresh
`artifacts/native/<target>/` directory. A rerun needs a new staging directory
(use `--output`) or deliberate removal of the previous generated bundle.

Each bundle contains the native library and provenance, target-specific dependency
notices, Rust redistribution notices, and corresponding sources for MPL dependencies.
Review notice-generator warnings and update `rust/about.toml` deliberately when
dependencies introduce license requirements.

### Assemble the release ZIP

Collect all three target directories under `artifacts/native/` on a host with
the same source revision. Assembly requires only Python, not Rust or platform
toolchains:

```sh
python scripts/package-plugin.py package
```

Outputs:

- `artifacts/releases/PostHog-vX.Y.Z.zip`
- `artifacts/releases/PostHog-vX.Y.Z.zip.sha256`

Assembly rechecks every target's native provenance and refuses missing libraries
or notices. It cannot produce a partial-platform release.

The ZIP contains one `PostHog/` directory with the unchanged three-platform
descriptor, C++ source, all three native libraries, provenance manifests, SDK
changelog, install instructions and license material. `THIRD_PARTY_NOTICES.txt`
indexes target-specific texts under `ThirdPartyNotices/`; `RustNotices/` and
`ThirdPartySources/` are also organized by target.

### Validation gates

Before tagging, test the **extracted combined ZIP** in clean UE 5.8 C++ consumers
without building Rust in those consumers. Validate Editor and packaged
Development/Shipping on each supported platform using the gates in
[CI documentation](ci.md). Confirm Linux libc/sysroot compatibility and Windows
CRT/toolchain linkage against the intended Unreal toolchains.

Native runner success establishes compilation, bridge tests and package
consistency, not Unreal/runtime compatibility, a minimum Linux distribution
version, or screenshot privacy. Existing Mac runtime evidence does not validate
Windows/Linux consumers.

## Publish

After the preparation PR merges, tag that exact commit with `v` followed by the
SDK version, then push the tag over SSH. For example, for version 0.1.0:

```sh
git tag -a v0.1.0 <merged-release-commit> -m "PostHog Unreal v0.1.0"
git push origin refs/tags/v0.1.0
```

The Release workflow:

1. Validates main ancestry, version/changelog consistency and tooling tests,
   then pins the source commit for all subsequent jobs.
2. Builds and tests each native library on its native runner, generates notices
   and uploads a target-specific bundle.
3. Downloads all three bundles and assembles the single plugin ZIP and checksum.
   Any failed target blocks publication.
4. Uploads both assets to a draft GitHub Release, then publishes it with the
   SDK's Sampo changelog entry as release notes.

While `IsBetaVersion` is true (or the version has a prerelease suffix), the release
is marked prerelease and does not become the latest stable release.

To retry a failed publication, rerun the workflow or dispatch it with the existing
tag. A retry completes a draft's assets before publishing. Published assets are
not overwritten; a published release missing expected assets requires manual
inspection. Do not move a published tag. Fix released code with a new changeset/version.

### Repository setup

- Create a protected `Release` environment with required reviewers.
- Restrict `v*` tag creation to maintainers through a repository ruleset.
- Grant the publication job `contents: write` for GitHub Releases. Validation and
  native build jobs have read-only repository permissions.
- Permit the `macos-15`, `ubuntu-22.04` and `windows-2022` hosted runners and
  GitHub Actions artifact transfer.

## Install the GitHub package

Download the plugin ZIP asset, not GitHub's automatic whole-repository source ZIP.
Verify its checksum, then copy the extracted `PostHog/` directory into
`<Project>/Plugins/`. Rebuild your C++ project with the matching UE 5.8 platform
toolchain. Rust and Cargo are not needed. A Blueprint-only project must first add
a C++ class and use the Unreal C++ toolchain.

See [the SDK README](../README.md) for setup and API usage.
