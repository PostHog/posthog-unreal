# Automated releases and plugin distribution

The SDK is experimental. The release workflow publishes **prereleases**, never marks them as the latest stable release, and does not publish anything to Fab.

Tags exactly match the SDK version, **without a `v` prefix**: use `0.1.0`, not `v0.1.0`.

The entire PostHog Unreal SDK has one version, tag and GitHub Release. The release asset is one installable plugin ZIP: the Unreal C++ source plus matching prebuilt Rust static libraries for all three desktop targets.

| Target | Native build runner | Bundled library |
| --- | --- | --- |
| Mac arm64 | macOS 15, deployment target macOS 14 | `libposthog_unreal_ffi.a` |
| Linux x64 | Ubuntu 22.04 | `libposthog_unreal_ffi.a` |
| Windows x64 | Windows Server 2022 / MSVC | `posthog_unreal_ffi.lib` |

The adapter targets UE 5.8. Consumers compile the C++ with Unreal, not Cargo. Unreal links only the library for the selected platform.

## How it works

| Trigger | Result |
| --- | --- |
| Open a pull request or push to `main` | Run [SDK tests](.github/workflows/sdk-tests.yml), including native builds on all three runners; no release is published. |
| Merge a releasable `.changeset/*.md` intent into `main` | Prepare and verify an uncommitted version/changelog patch, then request approval to commit and publish it. |
| Manually run Release on `main` | Process any pending change intents through the same publishing pipeline; **not a dry run**. |
| Push a version tag | No release workflow; tags are created by the pipeline. |
| Publish to Fab | Not supported yet; see [Fab distribution](#fab-distribution). |

The [Release workflow](.github/workflows/release.yml) serializes releases. Queued runs check out the latest `main` and consume only still-pending change intents; runs with none skip publication. pnpm's ledger prevents already-consumed intents from being released again.

The job sequence is `prepare-candidate` → `verify` → `notify-approval-needed` → `publish`, following the Godot SDK's prepare, verify, approve, then commit/publish flow:

1. In a read-only job, pnpm 11.13.1 applies pending change intents locally, updates `package.json` and `CHANGELOG.md`, and records consumed intents in `.changeset/ledger.yaml`. `scripts/bump_version.py` synchronizes `PostHog.uplugin` (`VersionName`, and increments the integer `Version`), the Rust bridge crate version and its `rust/Cargo.lock` entry. The bridge reports its crate version as `$lib_version`, so all of them must match. The job uploads a patch and records its SHA-256 checksum, source commit, and resulting Git tree SHA. **No version commit or remote change is made.**
2. The reusable [SDK tests workflow](.github/workflows/sdk-tests.yml) checks out the source commit and applies the exact patch in every job. `scripts/release_candidate.py` verifies the patch checksum, resulting tree, and versions. Tooling tests run on Ubuntu and Windows. On macOS 15, Ubuntu 22.04 and Windows 2022, the `bundle` job builds the library and runs the bridge tests (`scripts/build-native.py <target> --test`), generates dependency notices with cargo-about, stages a native bundle (`scripts/package-plugin.py stage`), and uploads it as `posthog-native-<target>-<tree-sha>`. An `assemble` job then combines the three bundles with the candidate source into `posthog-unreal-<version>.zip` (`scripts/package-plugin.py package` rechecks each target's provenance and refuses missing libraries or notices), validates it and writes `SHA256SUMS` with `scripts/release.py`, prints the checksum in the workflow summary, reports it as a workflow output, and uploads both as `posthog-unreal-candidate`. The protected engine/mock job does not run for release candidates.
3. Slack receives an approval request. Complete the [manual Unreal Engine validation](#manual-unreal-engine-validation) on the `posthog-unreal-candidate` ZIP, review the candidate patch, workflow summary, and CI results, then approve the **single `Release` deployment** in GitHub.
4. After approval, `publish` reapplies the verified patch, downloads `posthog-unreal-candidate`, requires the ZIP's SHA-256 to equal the checksum the `assemble` job reported, and revalidates the ZIP. A missing, different or invalid ZIP fails here, before anything changes on `main`.
5. Only then does `publish` obtain the environment-protected App key and create a GitHub-signed version commit on `main`. The expected-parent check rejects concurrent changes to `main`. A retry can reuse an already-created signed commit only when its parent, tree, and release message match this candidate exactly.
6. The same approved job checks out the signed commit and requires its Git tree to match the tested candidate, so the ZIP's sources, version and `Cargo.lock` provenance match the released commit. No source changes are made after verification.
7. The job verifies the checksums, creates the unprefixed tag at the signed commit, and publishes the experimental prerelease. An existing tag is accepted only if it resolves to the same commit. Release creation does not overwrite an existing release.

No other job has the App credentials, and there is no second environment or approval. The Slack approval thread receives messages and reactions for approval (✅), successful publication (🚀), or rejection (🚫). Failures and cancellations are reported by the [failure notification workflow](#failed-or-cancelled-release). Rejected approvals do not send a PostHog failure event; workflow failures still do.

## One-time repository setup

Configure these before enabling releases. The YAML does not create any of them:

- Create a **`Release` environment** with required reviewers and a selected deployment branch policy allowing only the `main` branch, not tags. Both controls matter: reviewers enforce approval, and the branch policy prevents modified branch workflows from accessing the key. Do not rely on the workflow's editable `if:` condition alone.
- Install a dedicated releaser GitHub App on `PostHog/posthog-unreal` with **Contents: write**. Store its client ID as `GH_APP_POSTHOG_UNREAL_RELEASER_APP_ID` and private key as `GH_APP_POSTHOG_UNREAL_RELEASER_PRIVATE_KEY` as **environment secrets only in `Release`**. Do not make these credentials available as repository or organization secrets. Remove any such copies: adding environment secrets does not prevent a branch workflow from reading a repository/organization copy. App tokens are short-lived; no personal access token is needed.
- Allow that App to bypass the `main` rules requiring a pull request and status checks for its automated version commit. Keep the signed-commit requirement: the GitHub commit API signs the App's commits.
- Provide `SLACK_CLIENT_LIBRARIES_BOT_TOKEN` and `POSTHOG_PROJECT_API_KEY` as repository/organization secrets, and `SLACK_APPROVALS_CLIENT_LIBRARIES_CHANNEL_ID` and `GROUP_CLIENT_LIBRARIES_SLACK_GROUP_ID` as repository/organization variables, matching the other SDK pipelines. The Slack bot needs `chat:write` and `reactions:write` scopes.
- Configure a tag ruleset preventing updates/deletion of `[0-9]*.[0-9]*.[0-9]*` tags and enable GitHub release immutability. Allow the releaser App to create tags, but never move or reuse one.

The workflow's default token has read-only repository access. Preparation and verification cannot commit or publish. Only the approved `publish` job obtains a scoped App token. Neither releases nor pull-request CI need an SSH key: Cargo fetches the public `posthog-rs` pin over HTTPS. The separate protected `sdk-ci` engine/mock configuration is described in [CI documentation](docs/ci.md#github-actions) and is not required for releases.

## Ship a change

Do **not** edit versions or the changelog manually. Add a change intent in the feature/fix PR:

```markdown
---
'posthog-unreal': patch
---

Describe the user-visible change.
```

`posthog-unreal` covers all SDK changes, including C++, Blueprint APIs, the Rust bridge and packaging. Use `patch`, `minor`, or `major`; pnpm combines pending intents and applies the highest required bump. With the pinned pnpm installed, `pnpm change posthog-unreal --bump patch --summary "Describe the change"` can generate the file. pnpm removes consumed intent files and records their entries in the ledger.

Run the checks in [CI documentation](docs/ci.md#local-commands), including `python -m unittest discover -s Tests -p 'test_*.py'` and `node --test scripts/release-failure-notification.test.cjs`, then merge the PR. Follow the Release run and Slack approval thread. Before approving, review the candidate patch, proposed version/changelog, CI results, and experimental/platform limitations.

To process pending intents without a new push:

```sh
gh workflow run release.yml --ref main
```

There is no publishing dry-run dispatch. To preview the bump locally without committing it, run `pnpm version -r` and `python scripts/bump_version.py` on a clean checkout, inspect the diff, then discard it with `git restore --staged --worktree .` (pnpm requires a clean working tree).

## Manual Unreal Engine validation

Hosted CI cannot run Unreal Engine: native jobs establish compilation, bridge tests and package consistency, not Unreal/runtime compatibility, a minimum Linux distribution version, or screenshot privacy. **Do not approve a release until a maintainer has validated the candidate ZIP in UE 5.8 on at least one bundled platform and every gate run on it passed.**

Replace the upper-case placeholders below with values from the Release run's workflow summary. Download the exact candidate ZIP; publication uploads these same bytes:

```sh
gh run download RUN_ID --name posthog-unreal-candidate --dir artifacts/candidate/RUN_ID
(cd artifacts/candidate/RUN_ID && shasum -a 256 --check SHA256SUMS)
```

Compare the checksum with the run's workflow summary.

For the engine gates, check out the candidate source:

```sh
gh run download RUN_ID --name release-candidate-SOURCE_SHA --dir artifacts/release-candidate
git fetch origin main && git checkout --detach SOURCE_SHA
python scripts/release_candidate.py artifacts/release-candidate/release.patch \
  --source SOURCE_SHA --sha256 PATCH_SHA256 --tree TREE_SHA --version VERSION
```

Then, on each bundled platform you can validate:

- Extract `artifacts/candidate/RUN_ID/posthog-unreal-<version>.zip` into a clean UE 5.8 C++ consumer's `Plugins/` directory, without building Rust in that consumer. Build the Editor and packaged Development/Shipping targets, and confirm events reach the SDK mock server.
- Run the engine gates in [CI documentation](docs/ci.md#local-commands) at the candidate source: `scripts/build-test-host.py`, `Tests/native_integration.py --editor`, and `Tests/replay_smoke.py`, plus the packaged-runtime fixture.
- Confirm Linux libc/sysroot compatibility and Windows CRT/toolchain linkage against the intended Unreal toolchains. Mac runtime evidence does not validate Windows/Linux consumers.

Record the platforms, configurations and results in the Slack approval thread, and name each bundled platform that was not validated. As with the Godot SDK's experimental prereleases, a platform may ship unvalidated only while the README [status](README.md#status) lists it as unvalidated; the release notes link there. Reject the deployment if any gate fails. Restore your checkout with `git restore --staged --worktree . && git checkout main` afterwards.

## Inspect assets and recover failures

```sh
gh run list --workflow release.yml --branch main
gh run watch RUN_ID --exit-status
gh run download RUN_ID --name release-assets --dir artifacts/release-review/RUN_ID
```

Before approval, download `release-candidate-SOURCE_SHA` to inspect `release.patch`; the workflow summary gives its checksum and resulting tree SHA. `posthog-unreal-candidate` is the exact ZIP and `SHA256SUMS` publication will upload; the `posthog-native-<target>-<tree-sha>` artifacts are the bundles it was assembled from.

After approval, `release-assets` contains `posthog-unreal-<version>.zip` and `SHA256SUMS`. Check them with `sha256sum --check SHA256SUMS` inside the downloaded directory (macOS: `shasum -a 256 --check SHA256SUMS`). Its checksum matches the `posthog-unreal-candidate` ZIP you validated. Its release download URL is unavailable until publication.

A failed candidate check or rejected approval leaves `main`, versions, and remote intents unchanged. If `main` moves before committing the approved patch, the workflow fails safely. Dispatch a fresh run on `main` to prepare and verify a new candidate before approving again. Once `assemble` has uploaded `posthog-unreal-candidate`, do not use **Re-run all jobs** or re-run a `bundle` or `assemble` job: each attempt's artifacts stay in the run under the same name, so a download can return an earlier attempt's ZIP, and `publish` then stops at its checksum check. Dispatch a fresh run instead.

If publication fails **after the version commit**, fix the underlying infrastructure issue and **re-run failed jobs in the original run**, preserving the verified patch, ZIP and metadata. The job recognizes and reuses its already-created signed commit if it is still the tip of `main` and its parent, tree, and message match. It does not create another version commit. A fresh dispatch sees consumed intents and will not republish that version. If `main` has advanced again or a code fix is needed, release a new version rather than overwriting history.

Inspect any existing tag/release before retrying publication. Never replace a published release, overwrite its assets, move a tag, or disable signing to work around a failure. Candidate patches, native bundles and the candidate ZIP are retained for five days. If an artifact expires before any commit, dispatch a fresh candidate; if the version commit already exists, ship a new version rather than synthesizing an unverified patch or rebuilding bundles outside the verified run.

### Failed or cancelled release

The independent [release failure notification workflow](.github/workflows/release-failure-notification.yml) checks every completed main-branch release attempt. It alerts if a job failed, timed out, or was cancelled, and if the publish job never ran in a failed run. This covers runner startup failures and runs that GitHub Actions ends early, which the in-run result job cannot report. It only acts on runs that a push or a manual dispatch started in this repository.

It stays quiet when the in-run result job already delivered the rejection reply. It alerts for any failed job, and if the GitHub release exists but the released Slack reply was skipped. Once the release exists, alerts describe follow-up problems rather than claiming the release failed.

Cancelled runs with no jobs are ignored, including superseded queued runs. A rejected approval is also ignored when the rejection reply was delivered and no other job failed.

Run the notifier tests with `node --test scripts/release-failure-notification.test.cjs`.

If job inspection still fails after three retries, the workflow posts a fallback alert with the overall run status and a logs link so the release can be checked manually.

Alerts reply to the approval thread. The release workflow saves the thread timestamp in a run artifact for 30 days, independently of approval. Failed-job reruns reuse the latest available thread. If the timestamp is missing or invalid, the alert is a new channel message. The workflow must be on the default branch to run.

## Install the plugin ZIP

Download `posthog-unreal-<version>.zip` from the GitHub Release, not GitHub's automatic whole-repository source archives. Verify it against `SHA256SUMS`, then copy the extracted `PostHog/` directory into `<Project>/Plugins/`. Rebuild your C++ project with the matching UE 5.8 platform toolchain. Rust and Cargo are not needed. A Blueprint-only project must first add a C++ class and use the Unreal C++ toolchain.

The ZIP contains one `PostHog/` directory with the unchanged three-platform descriptor, C++ source, all three native libraries, provenance manifests, changelog, install instructions and license material. `THIRD_PARTY_NOTICES.txt` indexes target-specific texts under `ThirdPartyNotices/`; `RustNotices/` and `ThirdPartySources/` (corresponding sources for MPL dependencies) are also organized by target. Review cargo-about warnings and update `rust/about.toml` deliberately when dependencies introduce license requirements.

See [the SDK README](README.md) for setup and API usage.

## Fab distribution

Publishing to Fab is out of scope for now. There is no automated submission or stored Fab credential in this repository. Distribution is the GitHub Release ZIP only.

## Future stable releases

Stable publication is deliberately unsupported. Before the first stable release, make a separately reviewed change to the experimental warnings and publication policy, including the `--prerelease` and `--latest=false` flags. Do not manually relabel an experimental release.
