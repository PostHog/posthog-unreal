import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("release", ROOT / "scripts/release.py")
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.crate = self.root / "rust/posthog-unreal-ffi"
        self.crate.mkdir(parents=True)
        (self.root / "rust/.sampo/changesets").mkdir(parents=True)
        self.write_versions("0.1.0")
        (self.root / "PostHog.uplugin").write_text(json.dumps({
            "VersionName": "0.1.0", "Version": 2, "IsBetaVersion": True,
        }))
        (self.crate / "CHANGELOG.md").write_text(
            "# Changelog\n\n## 0.1.0 — 2026-01-01\n\n### Minor changes\n\n- New feature.\n\n"
            "## 0.0.1 — 2025-01-01\n\n- Older change.\n", encoding="utf-8")

    def write_versions(self, version):
        (self.crate / "Cargo.toml").write_text(f'[package]\nname = "posthog-unreal-ffi"\nversion = "{version}"\n')
        (self.root / "rust/Cargo.lock").write_text(f'[[package]]\nname = "posthog-unreal-ffi"\nversion = "{version}"\n')

    def test_matching_release(self):
        self.assertEqual(release.check(self.root, "v0.1.0")[0], "0.1.0")

    def test_tag_must_match(self):
        for tag in ("v0.0.1", "0.1.0", "main", "v0.1.0-extra"):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                release.check(self.root, tag)

    def test_descriptor_must_match(self):
        self.write_versions("0.2.0")
        with self.assertRaisesRegex(ValueError, "VersionName"):
            release.check(self.root)

    def test_lockfile_must_match(self):
        (self.root / "rust/Cargo.lock").write_text('[[package]]\nname = "posthog-unreal-ffi"\nversion = "0.0.1"\n')
        with self.assertRaisesRegex(ValueError, "Cargo.lock"):
            release.check(self.root)

    def test_pending_changeset_blocks_publication(self):
        (self.root / "rust/.sampo/changesets/fix.md").write_text("pending")
        with self.assertRaisesRegex(ValueError, "pending changesets"):
            release.check(self.root)

    def test_only_current_notes_are_published(self):
        self.assertEqual(release.release_notes(self.root, "0.1.0"), "### Minor changes\n\n- New feature.\n")
        with self.assertRaisesRegex(ValueError, "No changelog entry"):
            release.release_notes(self.root, "0.2.0")

    def test_prepare_syncs_descriptor_once(self):
        def sampo(*args, **kwargs):
            self.write_versions("0.2.0")
            return subprocess.CompletedProcess(args, 0)

        with patch.object(release.subprocess, "run", side_effect=sampo) as run:
            release.prepare(self.root)
            release.prepare(self.root)
        descriptor = json.loads((self.root / "PostHog.uplugin").read_text())
        self.assertEqual(descriptor["VersionName"], "0.2.0")
        self.assertEqual(descriptor["Version"], 3)
        self.assertTrue(descriptor["IsBetaVersion"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root / "rust")
        self.assertEqual(run.call_args.kwargs["env"]["SAMPO_RELEASE_BRANCH"], "main")

    def test_prepare_failure_is_not_silenced(self):
        with patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 1)):
            with self.assertRaises(subprocess.CalledProcessError):
                release.prepare(self.root)
        self.assertEqual(json.loads((self.root / "PostHog.uplugin").read_text())["Version"], 2)

    def test_no_changes_does_not_bump_descriptor(self):
        with patch.object(release.subprocess, "run", return_value=subprocess.CompletedProcess([], 2)):
            release.prepare(self.root)
        self.assertEqual(json.loads((self.root / "PostHog.uplugin").read_text())["Version"], 2)


if __name__ == "__main__":
    unittest.main()
