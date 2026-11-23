import json
from pathlib import Path
import shutil
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))

import bump_version  # noqa: E402
import release  # noqa: E402

VERSION_FILES = ("package.json", "PostHog.uplugin", "rust/posthog-unreal-ffi/Cargo.toml", "rust/Cargo.lock")


def copy_version_files(directory, version=None):
    """Copy the repository's real version declarations, optionally setting a new pnpm version."""
    for name in VERSION_FILES:
        path = directory / name
        path.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(ROOT / name, path)
    if version is not None:
        set_package_version(directory, version)


def set_package_version(directory, version):
    path = directory / "package.json"
    metadata = json.loads(path.read_text(encoding="utf-8"))
    metadata["version"] = version
    path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def read(self, name):
        return (self.root / name).read_text(encoding="utf-8")

    def test_repository_version_declarations_agree(self):
        package = json.loads((ROOT / "package.json").read_text(encoding="utf-8"))
        self.assertEqual(package["name"], "posthog-unreal")
        self.assertTrue(package["private"])
        self.assertEqual(release.get_version(), package["version"])

    def test_bump_updates_all_declarations_and_preserves_other_content(self):
        old = release.get_version()
        old_build = json.loads((ROOT / "PostHog.uplugin").read_text(encoding="utf-8"))["Version"]
        originals = {name: (ROOT / name).read_text(encoding="utf-8") for name in VERSION_FILES[1:]}
        # Versions far from the current one, so each bump really changes every declaration.
        for version in ("99.0.0", "99.1.0-rc.1", "100.0.0"):
            with self.subTest(version=version):
                copy_version_files(self.root, version)
                self.assertEqual(bump_version.bump_version(self.root), version)
                self.assertEqual(release.get_version(self.root), version)
                self.assertEqual(self.read("PostHog.uplugin"), originals["PostHog.uplugin"]
                                 .replace(f'"VersionName": "{old}"', f'"VersionName": "{version}"')
                                 .replace(f'"Version": {old_build},', f'"Version": {old_build + 1},'))
                crate = "rust/posthog-unreal-ffi/Cargo.toml"
                self.assertEqual(self.read(crate),
                                 originals[crate].replace(f'version = "{old}"', f'version = "{version}"', 1))
                bridge = 'name = "posthog-unreal-ffi"\nversion = '
                self.assertEqual(self.read("rust/Cargo.lock"), originals["rust/Cargo.lock"]
                                 .replace(f'{bridge}"{old}"', f'{bridge}"{version}"'))

    def test_bump_increments_the_plugin_build_number_once_per_version(self):
        build = json.loads((ROOT / "PostHog.uplugin").read_text(encoding="utf-8"))["Version"]
        copy_version_files(self.root, "99.3.0")
        bump_version.bump_version(self.root)
        bump_version.bump_version(self.root)
        self.assertEqual(json.loads(self.read("PostHog.uplugin"))["Version"], build + 1)

    def test_invalid_release_version_does_not_modify_declarations(self):
        for version in ("v0.2.0", "0.2", "01.2.0", "latest", "0.2.0\n"):
            with self.subTest(version=version):
                copy_version_files(self.root, version)
                originals = {name: self.read(name) for name in VERSION_FILES[1:]}
                with self.assertRaisesRegex(ValueError, "Invalid release version"):
                    bump_version.bump_version(self.root)
                self.assertEqual({name: self.read(name) for name in VERSION_FILES[1:]}, originals)

    def test_missing_declaration_does_not_partially_bump(self):
        copy_version_files(self.root, "0.2.0")
        lock = self.root / "rust/Cargo.lock"
        lock.write_text(self.read("rust/Cargo.lock").replace('name = "posthog-unreal-ffi"', 'name = "renamed"'),
                        encoding="utf-8")
        originals = {name: self.read(name) for name in VERSION_FILES[1:]}
        with self.assertRaisesRegex(ValueError, "Cargo.lock"):
            bump_version.bump_version(self.root)
        self.assertEqual({name: self.read(name) for name in VERSION_FILES[1:]}, originals)

    def test_mismatched_declarations_are_rejected(self):
        old = release.get_version()
        build = json.loads((ROOT / "PostHog.uplugin").read_text(encoding="utf-8"))["Version"]
        bridge = 'name = "posthog-unreal-ffi"\nversion = '
        for name, search, replacement in (
                ("PostHog.uplugin", f'"VersionName": "{old}"', '"VersionName": "99.9.9"'),
                ("PostHog.uplugin", f'"Version": {build},', f'"Version": "{build}",'),
                ("rust/posthog-unreal-ffi/Cargo.toml", f'version = "{old}"', 'version = "99.9.9"'),
                ("rust/Cargo.lock", f'{bridge}"{old}"', f'{bridge}"99.9.9"')):
            with self.subTest(name=name, replacement=replacement):
                copy_version_files(self.root)
                self.assertIn(search, self.read(name))
                (self.root / name).write_text(self.read(name).replace(search, replacement, 1), encoding="utf-8")
                with self.assertRaisesRegex(ValueError, Path(name).name):
                    release.get_version(self.root)

    def test_tag_must_match_the_unprefixed_version(self):
        for version, tag in (("0.1.0", "0.1.0"), ("0.2.0-rc.1", "0.2.0-rc.1")):
            copy_version_files(self.root, version)
            bump_version.bump_version(self.root)
            self.assertEqual(release.validate_tag(tag, self.root), version)
            for invalid in (f"v{version}", "0.1.1", "main", f"{version}/extra", f"{version}\n"):
                with self.subTest(tag=invalid), self.assertRaises(ValueError):
                    release.validate_tag(invalid, self.root)


if __name__ == "__main__":
    unittest.main()
