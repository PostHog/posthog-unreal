"""Exercise candidate verification and approved commits without contacting GitHub."""
import base64
import copy
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

from test_release import copy_version_files, set_package_version
import bump_version
import commit_release
import release
import release_candidate

REPOSITORY = "PostHog/posthog-unreal"


class ReleaseCandidateTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="posthog-candidate-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        copy_version_files(self.root, "0.0.9")
        bump_version.bump_version(self.root)
        self.write("CHANGELOG.md", "# Changelog\n")
        self.write(".changeset/ledger.yaml", "{}\n")
        self.write(".changeset/ready.md", "---\n'posthog-unreal': minor\n---\n\nPending release\n")
        self.write("Source/PostHog/PostHog.Build.cs", "// module rules\n")
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        self.git("config", "user.name", "Release test")
        self.git("config", "user.email", "release@example.com")
        self.git("add", ".")
        self.git("-c", "commit.gpgsign=false", "commit", "-qm", "source")
        self.source = self.git("rev-parse", "HEAD")

        # Simulate the read-only job: pnpm consumes the intent, then bump_version.py syncs declarations.
        set_package_version(self.root, "0.1.0")
        bump_version.bump_version(self.root)
        (self.root / ".changeset/ready.md").unlink()
        self.write(".changeset/ledger.yaml", "posthog-unreal@0.1.0:\n  intents:\n    - ready\n")
        self.write("CHANGELOG.md", "# Changelog\n\n## 0.1.0\n\n- Pending release\n")
        self.git("add", "-A")
        self.tree = self.git("write-tree")
        self.patch = self.root / "artifacts/release-candidate/release.patch"
        self.patch.parent.mkdir(parents=True)
        self.patch.write_bytes(subprocess.check_output(["git", "diff", "--cached", "--binary"], cwd=self.root))
        self.digest = hashlib.sha256(self.patch.read_bytes()).hexdigest()

        self.remote = {"sha": self.source, "parents": [], "commit": {"tree": {"sha": "0" * 40}, "message": "source"}}
        self.requests = []
        self.real_run = subprocess.run

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")

    def git(self, *args):
        return subprocess.check_output(["git", *args], cwd=self.root, text=True).strip()

    def github(self, command, **kwargs):
        if command[0] != "gh":
            return self.real_run(command, **kwargs)
        self.requests.append((command, kwargs))
        if command == ["gh", "api", f"repos/{REPOSITORY}/commits/main"]:
            response = self.remote
        elif command == ["gh", "api", "graphql", "--input", "-"]:
            if self.remote["sha"] != self.source:
                raise subprocess.CalledProcessError(1, command, stderr="main moved")
            response = {"data": {"createCommitOnBranch": {"commit": {"oid": "a" * 40}}}}
        else:
            self.fail(f"Unexpected GitHub command: {command}")
        return subprocess.CompletedProcess(command, 0, stdout=json.dumps(response))

    def commit(self, tree=None):
        with patch("commit_release.subprocess.run", side_effect=self.github):
            return commit_release.create_release_commit(self.root, REPOSITORY, "0.1.0", tree or self.tree)

    def mutations(self):
        return [request for command, request in self.requests if command[2] == "graphql"]

    def existing_release(self):
        return {
            "sha": "b" * 40,
            "parents": [{"sha": self.source}],
            "commit": {
                "tree": {"sha": self.tree},
                "message": "chore: release 0.1.0 [version bump]",
                "verification": {"verified": True},
            },
        }

    def apply(self, **overrides):
        arguments = dict(patch=self.patch, source=self.source, digest=self.digest, tree=self.tree, version="0.1.0")
        arguments.update(overrides)
        release_candidate.apply_candidate(**arguments, root=self.root)

    def restore_source(self):
        self.git("restore", "--source=HEAD", "--staged", "--worktree", "--", ".")

    def test_apply_preserves_source_commit_and_reproduces_candidate_tree(self):
        self.restore_source()
        self.apply()
        self.assertEqual(self.git("rev-parse", "HEAD"), self.source)
        self.assertEqual(self.git("write-tree"), self.tree)
        self.assertEqual(release.get_version(self.root), "0.1.0")
        self.assertFalse((self.root / ".changeset/ready.md").exists())

    def test_source_and_checksum_are_checked_before_applying(self):
        self.restore_source()
        for arguments in ({"source": "0" * 40}, {"digest": "0" * 64}):
            with self.subTest(arguments=arguments), self.assertRaisesRegex(ValueError, "source|checksum"):
                self.apply(**arguments)
            self.assertEqual(self.git("status", "--porcelain", "--untracked-files=no"), "")

    def test_applied_tree_and_version_must_match_candidate(self):
        for arguments in ({"tree": "0" * 40}, {"version": "0.1.1"}):
            with self.subTest(arguments=arguments):
                self.restore_source()
                with self.assertRaisesRegex(ValueError, "tree|version"):
                    self.apply(**arguments)

    def test_commit_uses_verified_tree_and_expected_parent(self):
        self.assertEqual(self.commit(), "a" * 40)
        mutations = self.mutations()
        self.assertEqual(len(mutations), 1)
        request = json.loads(mutations[0]["input"])["variables"]["input"]
        self.assertEqual(request["expectedHeadOid"], self.source)
        self.assertEqual(request["branch"], {"repositoryNameWithOwner": REPOSITORY, "branchName": "main"})
        self.assertEqual(request["message"], {"headline": "chore: release 0.1.0 [version bump]"})
        changes = request["fileChanges"]
        self.assertEqual(changes["deletions"], [{"path": ".changeset/ready.md"}])
        additions = {entry["path"]: base64.b64decode(entry["contents"]) for entry in changes["additions"]}
        self.assertEqual(set(additions), {"CHANGELOG.md", "package.json", ".changeset/ledger.yaml", "PostHog.uplugin",
                                          "rust/posthog-unreal-ffi/Cargo.toml", "rust/Cargo.lock"})
        for name, contents in additions.items():
            self.assertEqual(contents, (self.root / name).read_bytes())

    def test_main_records_the_commit_for_later_steps(self):
        output = self.root / "artifacts/outputs"
        environment = {"GH_REPO": REPOSITORY, "NEW_VERSION": "0.1.0", "EXPECTED_TREE_SHA": self.tree,
                       "GITHUB_OUTPUT": str(output)}
        with patch.dict(os.environ, environment), patch.object(release, "ROOT", self.root), \
                patch("commit_release.subprocess.run", side_effect=self.github), patch("sys.stdout"):
            commit_release.main()
        self.assertEqual(output.read_text(encoding="utf-8"), "sha=" + "a" * 40 + "\n")

    def test_unverified_tree_is_rejected_before_contacting_github(self):
        with self.assertRaisesRegex(ValueError, "tree"):
            self.commit(tree="0" * 40)
        self.assertEqual(self.requests, [])

    def test_changes_outside_the_release_files_are_rejected(self):
        self.write("Source/PostHog/PostHog.Build.cs", "// changed after verification\n")
        self.git("add", "Source")
        with self.assertRaisesRegex(ValueError, "Unexpected release change: Source/PostHog/PostHog.Build.cs"):
            self.commit(tree=self.git("write-tree"))
        self.assertEqual(self.requests, [])

    def test_stale_main_is_rejected_without_a_mutation(self):
        self.remote["sha"] = "c" * 40
        with self.assertRaisesRegex(ValueError, "main moved"):
            self.commit()
        self.assertEqual(self.mutations(), [])

    def test_retry_reuses_the_signed_release_commit(self):
        self.remote = self.existing_release()
        self.assertEqual(self.commit(), "b" * 40)
        self.assertEqual(self.mutations(), [])

    def test_retry_rejects_a_different_or_unsigned_commit(self):
        for field in ("parent", "tree", "message", "signature"):
            with self.subTest(field=field):
                self.remote = copy.deepcopy(self.existing_release())
                if field == "parent":
                    self.remote["parents"][0]["sha"] = "c" * 40
                elif field == "tree":
                    self.remote["commit"]["tree"]["sha"] = "c" * 40
                elif field == "message":
                    self.remote["commit"]["message"] = "other release"
                else:
                    self.remote["commit"]["verification"]["verified"] = False
                with self.assertRaisesRegex(ValueError, "main moved"):
                    self.commit()
                self.assertEqual(self.mutations(), [])


    def test_workflow_stages_only_files_the_release_commit_allows(self):
        workflow = (release.ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        staged = [line.split()[2:] for line in workflow.splitlines() if line.strip().startswith("git add ")]
        self.assertEqual(len(staged), 1)
        allowed = (commit_release.ALLOWED - {".changeset/ledger.yaml"}) | {".changeset"}
        self.assertEqual(set(staged[0]), allowed)


if __name__ == "__main__":
    unittest.main()
