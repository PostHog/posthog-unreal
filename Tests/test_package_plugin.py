import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name.replace("-", "_"), ROOT / "scripts" / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


PKG = load_script("package-plugin")
BUMP = load_script("bump-version")


def make_fake_repo(base, version="0.0.1", int_version=1):
    base.mkdir(parents=True, exist_ok=True)
    (base / "PostHog.uplugin").write_text(
        json.dumps({"FileVersion": 3, "Version": int_version, "VersionName": version}, indent=2) + "\n",
        encoding="utf-8",
    )
    (base / "LICENSE").write_text("MIT License\n", encoding="utf-8")
    (base / "rust/posthog-stateful-ffi").mkdir(parents=True)
    (base / "rust/posthog-stateful-ffi/Cargo.toml").write_text(
        f'[package]\nname = "posthog-stateful-ffi"\nversion = "{version}"\n',
        encoding="utf-8",
    )
    (base / "rust/Cargo.lock").write_text(
        f'[[package]]\nname = "posthog-stateful-ffi"\nversion = "{version}"\n',
        encoding="utf-8",
    )
    (base / "Source/PostHog/Private").mkdir(parents=True)
    (base / "Source/PostHog/Private/PostHogClient.cpp").write_text(
        f'posthog_stateful_config_set_sdk_identity(NativeCfg, "posthog-unreal", "{version}");\n',
        encoding="utf-8",
    )
    (base / "Source/PostHog/PostHog.Build.cs").write_text("// build\n", encoding="utf-8")
    (base / "Source/PostHogAnalytics/Public").mkdir(parents=True)
    (base / "Source/PostHogAnalytics/PostHogAnalytics.Build.cs").write_text("// analytics\n", encoding="utf-8")
    (base / "Source/PostHogAnalytics/Public/PostHogAnalyticsModule.h").write_text("#pragma once\n", encoding="utf-8")
    (base / "Source/ThirdParty/PostHogRust/include").mkdir(parents=True)
    (base / "Source/ThirdParty/PostHogRust/include/posthog_stateful.h").write_text("#pragma once\n", encoding="utf-8")
    (base / "Source/ThirdParty/PostHogRust/PostHogRust.Build.cs").write_text("// rust\n", encoding="utf-8")
    for target, library in PKG.TARGETS.items():
        lib_dir = base / "Source/ThirdParty/PostHogRust/lib" / target
        lib_dir.mkdir(parents=True)
        (lib_dir / library).write_bytes(f"lib-{target}".encode("utf-8"))


def make_fake_rust_docs(base):
    docs = base / "rust-docs"
    (docs / "licenses").mkdir(parents=True)
    (docs / "COPYRIGHT-library.html").write_text("Rust std copyright\n", encoding="utf-8")
    (docs / "COPYRIGHT.html").write_text("Rust toolchain copyright (should not be bundled)\n", encoding="utf-8")
    (docs / "licenses/Apache-2.0.txt").write_text("Apache-2.0\n", encoding="utf-8")
    (docs / "licenses/MIT.txt").write_text("MIT\n", encoding="utf-8")
    (docs / "licenses/GPL-3.0-or-later.txt").write_text("GPL (should not be bundled)\n", encoding="utf-8")
    return docs


class PackagePluginTests(unittest.TestCase):
    def test_repo_versions_match(self):
        version = PKG.get_version(ROOT)
        self.assertRegex(version, PKG.VERSION_PATTERN)

    def test_about_toml_has_top_level_accepted_licenses(self):
        text = (ROOT / "rust/about.toml").read_text(encoding="utf-8")
        try:
            import tomllib
        except ModuleNotFoundError:
            tomllib = None

        if tomllib is not None:
            cfg = tomllib.loads(text)
            self.assertTrue(cfg.get("ignore-dev-dependencies"))
            self.assertTrue(cfg.get("ignore-build-dependencies"))
            self.assertIsInstance(cfg.get("accepted"), list)
            self.assertTrue(cfg["accepted"])
            self.assertEqual(cfg.get("private"), {"ignore": True})
        else:
            # Verify accepted = [...] appears before any [section] table header.
            header = text.split("\n[", 1)[0]
            self.assertIn("accepted = [", header)
            self.assertIn("ignore-dev-dependencies = true", header)
            self.assertIn("ignore-build-dependencies = true", header)

    def test_bump_version_updates_all_declarations(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            make_fake_repo(repo, version="0.0.1", int_version=1)
            self.assertEqual(BUMP.bump_version("0.1.0-alpha.1", repo), "0.1.0-alpha.1")
            descriptor = json.loads((repo / "PostHog.uplugin").read_text(encoding="utf-8"))
            self.assertEqual(descriptor["VersionName"], "0.1.0-alpha.1")
            self.assertEqual(descriptor["Version"], 2)
            self.assertEqual(PKG.get_version(repo), "0.1.0-alpha.1")

            # Idempotent when bumping to the same version.
            self.assertEqual(BUMP.bump_version("0.1.0-alpha.1", repo), "0.1.0-alpha.1")
            descriptor = json.loads((repo / "PostHog.uplugin").read_text(encoding="utf-8"))
            self.assertEqual(descriptor["Version"], 2)

    def test_version_mismatch_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            make_fake_repo(repo, version="0.0.1")
            (repo / "Source/PostHog/Private/PostHogClient.cpp").write_text(
                'posthog_stateful_config_set_sdk_identity(NativeCfg, "posthog-unreal", "9.9.9");\n',
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "PostHogClient.cpp"):
                PKG.get_version(repo)

    def test_stage_and_package_produces_deterministic_drop_in_zip(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            make_fake_repo(repo, version="0.2.0")
            rust_docs = make_fake_rust_docs(base)

            mpl_crate = base / "mpl-dep"
            mpl_crate.mkdir()
            (mpl_crate / "Cargo.toml").write_text('[package]\nname="webpki-roots"\nversion="1.0.0"\n', encoding="utf-8")
            (mpl_crate / "lib.rs").write_text("// mpl source\n", encoding="utf-8")
            (mpl_crate / ".cargo-ok").write_text("{}\n", encoding="utf-8")
            dev_mpl_crate = base / "dev-mpl-dep"
            dev_mpl_crate.mkdir()
            (dev_mpl_crate / "Cargo.toml").write_text('[package]\nname="dev-only"\nversion="1.0.0"\n', encoding="utf-8")
            (dev_mpl_crate / "lib.rs").write_text("// dev only\n", encoding="utf-8")
            metadata = {
                "resolve": {
                    "nodes": [
                        {
                            "id": "bridge-id",
                            "deps": [
                                {"pkg": "mpl-id", "dep_kinds": [{"kind": None}]},
                                {"pkg": "dev-mpl-id", "dep_kinds": [{"kind": "dev"}]},
                            ],
                        },
                        {"id": "mpl-id", "deps": []},
                        {"id": "dev-mpl-id", "deps": []},
                    ]
                },
                "packages": [
                    {
                        "id": "bridge-id",
                        "name": "posthog-stateful-ffi",
                        "version": "0.2.0",
                        "license": "MIT",
                        "manifest_path": str(repo / "rust/posthog-stateful-ffi/Cargo.toml"),
                    },
                    {
                        "id": "mpl-id",
                        "name": "webpki-roots",
                        "version": "1.0.0",
                        "license": "MPL-2.0",
                        "manifest_path": str(mpl_crate / "Cargo.toml"),
                    },
                    {
                        "id": "dev-mpl-id",
                        "name": "dev-only",
                        "version": "1.0.0",
                        "license": "MPL-2.0",
                        "manifest_path": str(dev_mpl_crate / "Cargo.toml"),
                    },
                ],
            }

            notices = base / "notices.txt"
            notices.write_text("Notice text\n", encoding="utf-8")
            native_dir = base / "native"
            for target in PKG.TARGETS:
                PKG.stage_native(repo, target, native_dir, notices, metadata, rust_docs)

            out1 = base / "out1"
            out2 = base / "out2"
            zip1 = PKG.package_plugin(repo, native_dir, out1)
            zip2 = PKG.package_plugin(repo, native_dir, out2)

            self.assertEqual(zip1.name, "posthog-unreal-0.2.0.zip")
            self.assertEqual(zip1.read_bytes(), zip2.read_bytes())

            sums = (out1 / "SHA256SUMS").read_text(encoding="utf-8")
            expected_hash = hashlib.sha256(zip1.read_bytes()).hexdigest()
            self.assertEqual(sums, f"{expected_hash}  posthog-unreal-0.2.0.zip\n")

            with zipfile.ZipFile(zip1) as zf:
                names = set(zf.namelist())
            self.assertIn("PostHog/PostHog.uplugin", names)
            self.assertIn("PostHog/LICENSE", names)
            self.assertIn("PostHog/README.md", names)
            self.assertIn("PostHog/THIRD_PARTY_NOTICES.txt", names)
            self.assertIn("PostHog/Source/ThirdParty/PostHogRust/include/posthog_stateful.h", names)
            self.assertIn("PostHog/Source/ThirdParty/PostHogRust/PostHogRust.Build.cs", names)
            for target, lib in PKG.TARGETS.items():
                self.assertIn(f"PostHog/Source/ThirdParty/PostHogRust/lib/{target}/{lib}", names)
                self.assertIn(f"PostHog/ThirdPartyNotices/{target}.txt", names)
                self.assertIn(f"PostHog/RustNotices/{target}/COPYRIGHT-library.html", names)
                self.assertIn(f"PostHog/RustNotices/{target}/licenses/Apache-2.0.txt", names)
                self.assertIn(f"PostHog/RustNotices/{target}/licenses/MIT.txt", names)
                self.assertNotIn(f"PostHog/RustNotices/{target}/COPYRIGHT.html", names)
                self.assertNotIn(f"PostHog/RustNotices/{target}/licenses/GPL-3.0-or-later.txt", names)
                self.assertIn(f"PostHog/ThirdPartySources/{target}/webpki-roots-1.0.0/lib.rs", names)
                self.assertNotIn(f"PostHog/ThirdPartySources/{target}/webpki-roots-1.0.0/.cargo-ok", names)
                self.assertNotIn(f"PostHog/ThirdPartySources/{target}/dev-only-1.0.0/lib.rs", names)

    def test_validate_tag(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            make_fake_repo(repo, version="0.2.0")
            self.assertEqual(PKG.validate_tag("0.2.0", repo), "0.2.0")
            with self.assertRaisesRegex(ValueError, "without a v prefix"):
                PKG.validate_tag("v0.2.0", repo)
            with self.assertRaisesRegex(ValueError, "must match"):
                PKG.validate_tag("0.2.1", repo)
            for bad in ("0.1.0-.alpha", "0.1.0-alpha..1", "0.1.0-01"):
                with self.assertRaisesRegex(ValueError, "Invalid semver"):
                    BUMP.bump_version(bad, repo)

    def test_missing_platform_bundle_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            make_fake_repo(repo, version="0.2.0")
            rust_docs = make_fake_rust_docs(base)
            notices = base / "notices.txt"
            notices.write_text("Notice text\n", encoding="utf-8")
            metadata = {"resolve": {"nodes": []}, "packages": []}
            native_dir = base / "native"
            PKG.stage_native(repo, "aarch64-apple-darwin", native_dir, notices, metadata, rust_docs)
            with self.assertRaisesRegex(ValueError, "Missing native bundle"):
                PKG.package_plugin(repo, native_dir, base / "out")

    def test_unexpected_file_in_native_bundle_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            make_fake_repo(repo, version="0.2.0")
            rust_docs = make_fake_rust_docs(base)
            notices = base / "notices.txt"
            notices.write_text("Notice text\n", encoding="utf-8")
            metadata = {"resolve": {"nodes": []}, "packages": []}
            native_dir = base / "native"
            for target in PKG.TARGETS:
                PKG.stage_native(repo, target, native_dir, notices, metadata, rust_docs)
            (native_dir / "aarch64-apple-darwin" / "unexpected.txt").write_text("nope\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "Unexpected file in native bundle"):
                PKG.package_plugin(repo, native_dir, base / "out")

    def test_missing_rust_dual_license_is_rejected(self):
        with tempfile.TemporaryDirectory() as tmp:
            base = Path(tmp)
            repo = base / "repo"
            make_fake_repo(repo, version="0.2.0")
            rust_docs = make_fake_rust_docs(base)
            (rust_docs / "licenses/MIT.txt").unlink()
            notices = base / "notices.txt"
            notices.write_text("Notice text\n", encoding="utf-8")
            metadata = {"resolve": {"nodes": []}, "packages": []}
            with self.assertRaisesRegex(ValueError, "missing standard-library notices"):
                PKG.stage_native(repo, "aarch64-apple-darwin", base / "native", notices, metadata, rust_docs)


if __name__ == "__main__":
    unittest.main()
