import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

from test_release import ROOT, release

spec = importlib.util.spec_from_file_location("package_plugin", ROOT / "scripts/package-plugin.py")
packaging = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packaging)


class PluginPackageTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.native_root = self.root / "native"
        self.write("rust/posthog-unreal-ffi/Cargo.toml", '[package]\nname = "posthog-unreal-ffi"\nversion = "0.1.0"\n')
        self.write("rust/Cargo.lock", '[[package]]\nname = "posthog-unreal-ffi"\nversion = "0.1.0"\n')
        self.write("package.json", json.dumps({"name": "posthog-unreal", "version": "0.1.0", "private": True}))
        self.write("CHANGELOG.md", "# Changelog\n\n## 0.1.0\n\n### Minor Changes\n\n- SDK feature.\n")
        self.write("PostHog.uplugin", json.dumps({
            "VersionName": "0.1.0", "Version": 2, "SupportedTargetPlatforms": ["Mac", "Win64", "Linux"],
            "Modules": [{"Name": "PostHog", "PlatformAllowList": ["Mac", "Win64", "Linux"]}],
        }))
        self.write("Source/PostHog/Public/PostHogClient.h", "// C++ header")
        self.write("Source/PostHog/Private/PostHogClient.cpp", "// C++ source")
        self.write("Source/PostHog/PostHog.Build.cs", "// module rules")
        self.write("Source/PostHogAnalytics/PostHogAnalytics.Build.cs", "// Analytics module rules")
        self.write("Source/ThirdParty/PostHogRust/PostHogRust.Build.cs", "// native module rules")
        self.write("Source/ThirdParty/PostHogRust/include/posthog_unreal.h", "#define PHU_ABI_VERSION 3u\n")
        self.write("LICENSE", "SDK license")
        self.notices = self.write("notices.txt", "Dependency licenses — Unicode text")
        self.rust_docs = self.root / "rust-docs"
        self.write("rust-docs/COPYRIGHT-library.html", "Rust library copyright notices")
        self.write("rust-docs/licenses/MIT.txt", "Rust license text")
        self.write("dependency/Cargo.toml", "// dependency manifest")
        self.write("dependency/src/lib.rs", "// corresponding MPL source")
        self.metadata = {"resolve": {"nodes": [{"id": "mpl-dependency"}]}, "packages": [{
            "id": "mpl-dependency", "name": "option-ext", "version": "0.2.0", "license": "MPL-2.0",
            "manifest_path": str(self.root / "dependency/Cargo.toml"),
        }]}
        for target, filename in packaging.TARGETS.items():
            library = self.write(f"Source/ThirdParty/PostHogRust/lib/{target}/{filename}", target + " library")
            manifest = {
                "target": target, "bridge_version": "0.1.0", "abi_version": 3,
                "sha256": packaging.sha256(library),
                "lockfile_sha256": packaging.sha256(self.root / "rust/Cargo.lock"),
            }
            self.write(f"Source/ThirdParty/PostHogRust/lib/{target}/manifest.json", json.dumps(manifest))
            self.stage(target, self.native_root)

    def write(self, name, text):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def stage(self, target, output):
        return packaging.stage_native(self.root, target, output, self.notices, self.metadata, self.rust_docs)

    def package(self):
        return packaging.package_plugin(self.root, self.native_root, self.root / "output")

    def test_combined_plugin_layout_contains_all_libraries_and_licenses(self):
        original = (self.root / "PostHog.uplugin").read_bytes()
        archive = self.package()
        self.assertEqual(archive.name, "posthog-unreal-0.1.0.zip")
        with zipfile.ZipFile(archive) as package:
            self.assertIsNone(package.testzip())
            names = set(package.namelist())
            self.assertIn("PostHog/Source/PostHog/Public/PostHogClient.h", names)
            self.assertIn("PostHog/Source/PostHogAnalytics/PostHogAnalytics.Build.cs", names)
            for target, filename in packaging.TARGETS.items():
                library = f"Source/ThirdParty/PostHogRust/lib/{target}/{filename}"
                self.assertEqual(package.read("PostHog/" + library), (self.root / library).read_bytes())
                self.assertEqual(package.read(f"PostHog/ThirdPartyNotices/{target}.txt"), self.notices.read_bytes())
                self.assertIn(f"PostHog/RustNotices/{target}/COPYRIGHT-library.html", names)
                self.assertIn(f"PostHog/RustNotices/{target}/licenses/MIT.txt", names)
                self.assertIn(f"PostHog/ThirdPartySources/{target}/option-ext-0.2.0/src/lib.rs", names)
            self.assertIn("PostHog/THIRD_PARTY_NOTICES.txt", names)
            self.assertEqual(package.read("PostHog/CHANGELOG.md"), (self.root / "CHANGELOG.md").read_bytes())
            self.assertIn("PostHog/LICENSE", names)
            self.assertIn(b"you do not need Rust or Cargo", package.read("PostHog/README.md"))
            self.assertEqual(package.read("PostHog/PostHog.uplugin"), original)
        self.assertEqual((self.root / "PostHog.uplugin").read_bytes(), original)

    def test_excludes_checkout_and_unrelated_build_products(self):
        for name in (".env", "Tests/Unreal/Source/Host.Build.cs", "Examples/Example.uproject",
                     "Binaries/Mac/editor.dylib", "Source/PostHog/.DS_Store",
                     "Source/ThirdParty/PostHogRust/lib/x86_64-pc-windows-msvc/unrelated.lib"):
            self.write(name, "excluded")
        with zipfile.ZipFile(self.package()) as package:
            for name in package.namelist():
                self.assertNotEqual(package.read(name), b"excluded")

    def test_archive_is_repeatable_for_identical_inputs(self):
        first = self.package().read_bytes()
        self.assertEqual(first, self.package().read_bytes())

    def test_assembly_cli_requires_no_rust_toolchain(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        for name in ("release.py", "package-plugin.py"):
            shutil.copy2(ROOT / "scripts" / name, scripts / name)
        empty_path = self.root / "empty-path"
        empty_path.mkdir()
        result = subprocess.run([
            sys.executable, str(scripts / "package-plugin.py"), "package",
            "--native", str(self.native_root), "--output", str(self.root / "output"),
        ], env={**os.environ, "PATH": str(empty_path)}, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        with zipfile.ZipFile(self.root / "output/posthog-unreal-0.1.0.zip") as package:
            self.assertIsNone(package.testzip())
            for target, filename in packaging.TARGETS.items():
                self.assertIn(f"PostHog/Source/ThirdParty/PostHogRust/lib/{target}/{filename}", package.namelist())

    def test_native_provenance_must_match_at_staging_and_assembly(self):
        for target in packaging.TARGETS:
            manifest_path = Path(f"Source/ThirdParty/PostHogRust/lib/{target}/manifest.json")
            original = (self.root / manifest_path).read_bytes()
            for key, value in (("abi_version", 2), ("bridge_version", "0.0.1"), ("target", "wrong-target"), ("lockfile_sha256", "incorrect")):
                with self.subTest(target=target, key=key):
                    manifest = json.loads(original)
                    manifest[key] = value
                    (self.root / manifest_path).write_text(json.dumps(manifest), encoding="utf-8")
                    (self.native_root / target / manifest_path).write_text(json.dumps(manifest), encoding="utf-8")
                    with self.assertRaisesRegex(ValueError, key):
                        self.stage(target, self.root / "other-native")
                    with self.assertRaisesRegex(ValueError, key):
                        self.package()
            (self.root / manifest_path).write_bytes(original)
            (self.native_root / target / manifest_path).write_bytes(original)

    def test_corrupt_library_is_rejected_at_staging_and_assembly(self):
        for target, filename in packaging.TARGETS.items():
            with self.subTest(target=target):
                library = Path(f"Source/ThirdParty/PostHogRust/lib/{target}/{filename}")
                original = (self.root / library).read_bytes()
                (self.root / library).write_bytes(b"corrupt library")
                (self.native_root / target / library).write_bytes(b"corrupt library")
                with self.assertRaisesRegex(ValueError, "sha256"):
                    self.stage(target, self.root / "other-native")
                with self.assertRaisesRegex(ValueError, "sha256"):
                    self.package()
                (self.root / library).write_bytes(original)
                (self.native_root / target / library).write_bytes(original)

    def test_partial_platform_matrix_cannot_produce_a_release(self):
        for target in packaging.TARGETS:
            with self.subTest(target=target):
                shutil.rmtree(self.native_root / target)
                with self.assertRaisesRegex(ValueError, target):
                    self.package()
                self.assertFalse((self.root / "output").exists())
                self.stage(target, self.native_root)

    def test_stage_requires_license_material_and_fresh_destination(self):
        target = next(iter(packaging.TARGETS))
        with self.assertRaises(FileExistsError):
            self.stage(target, self.native_root)
        self.notices.write_text("", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "notices"):
            self.stage(target, self.root / "other-native")
        self.notices.write_text("Dependency licenses", encoding="utf-8")
        (self.rust_docs / "COPYRIGHT-library.html").unlink()
        with self.assertRaisesRegex(ValueError, "rust-docs"):
            self.stage(target, self.root / "other-native")

    def test_assembly_requires_each_platforms_license_material(self):
        for target in packaging.TARGETS:
            with self.subTest(target=target):
                notice = self.native_root / target / f"ThirdPartyNotices/{target}.txt"
                original = notice.read_bytes()
                notice.unlink()
                with self.assertRaisesRegex(ValueError, "missing"):
                    self.package()
                notice.write_bytes(original)
                copyright_path = self.native_root / target / f"RustNotices/{target}/COPYRIGHT-library.html"
                copyright_data = copyright_path.read_bytes()
                copyright_path.unlink()
                with self.assertRaisesRegex(ValueError, "copyright"):
                    self.package()
                copyright_path.write_bytes(copyright_data)

    def test_assembly_rejects_unexpected_bundle_files(self):
        target = next(iter(packaging.TARGETS))
        self.write(f"native/{target}/.env", "private")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            self.package()


    def test_release_assets_checksum_the_assembled_plugin(self):
        archive = self.package()
        release.prepare_release("0.1.0", archive.parent, self.root)
        self.assertEqual((archive.parent / "SHA256SUMS").read_text(encoding="utf-8"),
                         f"{hashlib.sha256(archive.read_bytes()).hexdigest()}  {archive.name}\n")

    def test_release_assets_require_the_versioned_archive(self):
        (self.root / "output").mkdir()
        with self.assertRaises(FileNotFoundError):
            release.prepare_release("0.1.0", self.root / "output", self.root)
        self.package()
        with self.assertRaisesRegex(ValueError, "without a v prefix"):
            release.prepare_release("v0.1.0", self.root / "output", self.root)
        self.assertFalse((self.root / "output/SHA256SUMS").exists())

    def test_release_assets_reject_an_incomplete_or_mismatched_archive(self):
        archive = self.package()
        original = archive.read_bytes()
        library = "PostHog/Source/ThirdParty/PostHogRust/lib/x86_64-pc-windows-msvc/posthog_unreal_ffi.lib"
        manifest = "PostHog/Source/ThirdParty/PostHogRust/lib/aarch64-apple-darwin/manifest.json"

        def stale_manifest(files):
            files[manifest] = json.dumps({**json.loads(files[manifest]), "bridge_version": "0.0.9"}).encode()

        cases = {
            "missing": lambda files: files.pop(library),
            "sha256": lambda files: files.update({library: b"other library"}),
            "bridge_version": stale_manifest,
            "outside PostHog/": lambda files: files.update({"Other/file.txt": b"stray"}),
            "descriptor": lambda files: files.update({"PostHog/PostHog.uplugin": b'{"VersionName": "0.0.9"}'}),
        }
        for error, change in cases.items():
            with self.subTest(error=error):
                with zipfile.ZipFile(io.BytesIO(original)) as package:
                    files = {name: package.read(name) for name in package.namelist()}
                change(files)
                with zipfile.ZipFile(archive, "w") as package:
                    for name, data in files.items():
                        package.writestr(name, data)
                with self.assertRaisesRegex(ValueError, error):
                    release.prepare_release("0.1.0", archive.parent, self.root)
                self.assertFalse((archive.parent / "SHA256SUMS").exists())


if __name__ == "__main__":
    unittest.main()
