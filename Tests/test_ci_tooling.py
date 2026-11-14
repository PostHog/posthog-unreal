import base64
import configparser
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import stat
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from PIL import Image, ImageDraw
from replay_smoke import PROJECT, argument_parser, check_software_adapter, decode_pattern, launch_command, renderer_arguments

ROOT = Path(__file__).resolve().parents[1]


def load_script(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / "scripts" / (name + ".py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class ReplaySmokeTests(unittest.TestCase):
    def envelope(self, pattern=True):
        image = Image.new("RGB", (320, 240))
        if pattern:
            draw = ImageDraw.Draw(image)
            for index, color in enumerate(["red", "lime", "blue", "white"]):
                x, y = index % 2 * 160, index // 2 * 120
                draw.rectangle((x, y, x + 159, y + 119), fill=color)
        output = io.BytesIO()
        image.save(output, format="JPEG", quality=80)
        return {"properties": {"$snapshot_data": [{"type": 2, "data": {"wireframes": [{
            "width": 640, "height": 480, "base64": "data:image/jpeg;base64," + base64.b64encode(output.getvalue()).decode(),
        }]}}]}}

    def test_decodes_expected_rendered_content(self):
        self.assertTrue(decode_pattern(self.envelope()))

    def test_blank_startup_frame_does_not_pass(self):
        self.assertIsNone(decode_pattern(self.envelope(False)))

    def test_vertical_orientation_must_match_viewport(self):
        envelope = self.envelope()
        wireframe = envelope["properties"]["$snapshot_data"][0]["data"]["wireframes"][0]
        jpeg = base64.b64decode(wireframe["base64"].split(",", 1)[1])
        with Image.open(io.BytesIO(jpeg)) as original:
            image = original.convert("RGB")
        image = image.transpose(Image.Transpose.FLIP_TOP_BOTTOM)
        output = io.BytesIO(); image.save(output, format="JPEG", quality=80)
        wireframe["base64"] = "data:image/jpeg;base64," + base64.b64encode(output.getvalue()).decode()
        self.assertIsNone(decode_pattern(envelope))

    def test_unrelated_window_pattern_does_not_pass(self):
        envelope = self.envelope()
        output = io.BytesIO(); Image.new("RGB", (320, 240), "magenta").save(output, format="JPEG")
        envelope["properties"]["$snapshot_data"][0]["data"]["wireframes"][0]["base64"] = "data:image/jpeg;base64," + base64.b64encode(output.getvalue()).decode()
        self.assertIsNone(decode_pattern(envelope))

    def test_resize_filters_previous_surface_aspect(self):
        self.assertTrue(decode_pattern(self.envelope(), (4, 3)))
        self.assertIsNone(decode_pattern(self.envelope(), (16, 9)))

    def test_corrupt_image_does_not_pass(self):
        envelope = self.envelope()
        envelope["properties"]["$snapshot_data"][0]["data"]["wireframes"][0]["base64"] = "data:image/jpeg;base64,AA=="
        with self.assertRaises(OSError):
            decode_pattern(envelope)

    def test_wrong_aspect_ratio_is_rejected(self):
        envelope = self.envelope()
        envelope["properties"]["$snapshot_data"][0]["data"]["wireframes"][0]["height"] = 1000
        with self.assertRaises(AssertionError):
            decode_pattern(envelope)

    def test_software_arguments_render_instead_of_null_rhi(self):
        self.assertIn("-warp", renderer_arguments("Windows", "software"))
        self.assertIn("-AllowCPUDevices", renderer_arguments("Linux", "software"))
        self.assertEqual(renderer_arguments("Darwin", "native"), [])
        with self.assertRaises(ValueError):
            renderer_arguments("Darwin", "software")

    def test_editor_and_packaged_launch_commands(self):
        output = Path("evidence with spaces")
        common = ["-game", "-PostHogReplaySmoke", "-windowed", "-ResX=640", "-ResY=480",
                  "-ForceRes", "-unattended", "-nosound", "-nosplash", "-nop4", "-NoAssetRegistryCache",
                  "-NoScreenMessages", "-ExecCmds=t.MaxFPS 15", f"-abslog={output / 'editor.log'}",
                  "-stdout", "-FullStdOutLogOutput"]
        for system, renderer, rendering in (("Darwin", "native", []),
                                             ("Windows", "software", ["-dx12", "-warp", "-AllowSoftwareRendering"]),
                                             ("Linux", "software", ["-vulkan", "-AllowCPUDevices"])):
            for mode in ("--editor", "--game-executable"):
                for manual in (False, True):
                    with self.subTest(system=system, mode=mode, manual=manual):
                        executable = "consumer with spaces/UnrealEditor" if mode == "--editor" else "packaged host/PostHogTestHost"
                        arguments = [mode, executable, "--mock-server", "server.py", "--renderer", renderer]
                        if manual:
                            arguments.append("--manual-start")
                        args = argument_parser().parse_args(arguments)
                        self.assertEqual(args.timeout, 180)
                        expected = [executable, *([str(PROJECT)] if mode == "--editor" else []), *common, *rendering]
                        if manual:
                            expected.append("-PostHogReplayManualStart")
                        if system == "Linux":
                            expected = ["xvfb-run", "-a", "-s", "-screen 0 800x600x24", *expected]
                        command = launch_command(args, system, output)
                        self.assertEqual(command, expected)
                        if mode == "--game-executable":
                            self.assertFalse(any(argument.endswith(".uproject") for argument in command))

    def test_launch_requires_exactly_one_consumer(self):
        for arguments in ([], ["--editor", "UnrealEditor", "--game-executable", "PostHogTestHost"]):
            with self.subTest(arguments=arguments), patch("sys.stderr", new=io.StringIO()) as errors:
                with self.assertRaises(SystemExit) as raised:
                    argument_parser().parse_args(["--mock-server", "server.py", *arguments])
                self.assertEqual(raised.exception.code, 2)
                self.assertIn("--editor", errors.getvalue())
                self.assertIn("--game-executable", errors.getvalue())

    def test_hardware_adapter_cannot_pass_software_check(self):
        check_software_adapter({"rhi_adapter": "Microsoft Basic Render Driver", "rhi_vendor_id": 0x1414}, "Windows")
        check_software_adapter({"rhi_adapter": "llvmpipe (LLVM)"}, "Linux")
        for system in ("Windows", "Linux"):
            with self.assertRaises(AssertionError):
                check_software_adapter({"rhi_adapter": "NVIDIA", "rhi_vendor_id": 0x10DE}, system)


class BuildToolingTests(unittest.TestCase):
    def test_cargo_patch_quotes_paths(self):
        build = load_script("build-native")
        path = Path('folder with spaces') / 'core'
        patch = build.cargo_patch(path)
        self.assertIn('.posthog.path=', patch)
        self.assertEqual(json.loads(patch.split("=", 1)[1]), str(path))
        self.assertEqual(build.LIBRARIES["x86_64-pc-windows-msvc"], "posthog_unreal_ffi.lib")

    def test_native_build_installs_artifact_and_records_manifest(self):
        build = load_script("build-native")
        for target, filename in build.LIBRARIES.items():
            with self.subTest(target=target), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                core = root / "Rust core"
                (core / "src").mkdir(parents=True)
                (core / "Cargo.toml").write_text('[package]\nname = "posthog"\n')
                (core / "Cargo.lock").write_text("version = 4\n")
                (core / "src/lib.rs").write_text("// fixture\n")
                (root / "rust").mkdir()
                source = 'git+ssh://git@github.com/PostHog/posthog-rs.git?rev=' + 'a' * 40 + '#' + 'a' * 40
                (root / "rust/Cargo.lock").write_text('[[package]]\nname = "posthog"\nversion = "1.0.0"\nsource = "' + source + '"\n')
                target_dir = root / "build outputs"
                artifact = target_dir / target / "release" / filename
                artifact.parent.mkdir(parents=True)
                artifact.write_bytes(b"static library fixture")
                environment = {"CARGO_TARGET_DIR": str(target_dir)}
                metadata = json.dumps({"packages": [{"name": "posthog", "manifest_path": str(core / "Cargo.toml")}]})
                with patch.object(build, "ROOT", root), patch.dict(os.environ, environment, clear=True), \
                        patch("sys.argv", ["build-native.py", target, "--test"]), \
                        patch.object(build.subprocess, "run") as run, \
                        patch.object(build.subprocess, "check_output", side_effect=[metadata, "rustc fixture"]) as checked, \
                        patch("sys.stdout", new=io.StringIO()):
                    build.main()
                common = ["--manifest-path", str(root / "rust/Cargo.toml"),
                          "--locked", "--release", "--target", target]
                self.assertEqual(checked.call_args_list[0].args[0],
                                 ["cargo", "metadata", "--manifest-path", str(root / "rust/Cargo.toml"),
                                  "--locked", "--format-version", "1"])
                self.assertEqual([call.args[0] for call in run.call_args_list],
                                 [["cargo", "build", *common], ["cargo", "test", *common]])
                for call in run.call_args_list:
                    self.assertTrue(call.kwargs["check"])
                    self.assertEqual(call.kwargs["env"]["CARGO_NET_GIT_FETCH_WITH_CLI"], "true")
                    if target.endswith("apple-darwin"):
                        self.assertEqual(call.kwargs["env"]["MACOSX_DEPLOYMENT_TARGET"], "14.0")
                destination = root / "Source/ThirdParty/PostHogRust/lib" / target
                self.assertEqual((destination / filename).read_bytes(), artifact.read_bytes())
                manifest = json.loads((destination / "manifest.json").read_text())
                self.assertEqual(manifest["target"], target)
                self.assertEqual(manifest["sha256"], hashlib.sha256(artifact.read_bytes()).hexdigest())
                self.assertEqual(manifest["lockfile_sha256"], hashlib.sha256((root / "rust/Cargo.lock").read_bytes()).hexdigest())
                self.assertEqual(manifest["core_base"], "a" * 40)
                self.assertEqual(manifest["abi_version"], 3)
                self.assertEqual(manifest["core_package"], "posthog")
                self.assertEqual(manifest["core_source"], source)
                self.assertEqual(manifest["core_features"], ["tls", "session-replay"])
                self.assertFalse(manifest["development_override"])

    def test_host_uses_manual_code_signing(self):
        config = configparser.ConfigParser()
        config.read(ROOT / "Tests/Unreal/Config/DefaultEngine.ini")
        self.assertFalse(config.getboolean("/Script/MacTargetPlatform.XcodeProjectSettings", "bUseAutomaticCodeSigning"))

    def test_test_host_build_and_package_commands(self):
        build = load_script("build-test-host")
        for system, target, build_script, uat_script, architecture in (
                ("Darwin", "Mac", "Mac/Build.sh", "RunUAT.sh", "arm64"),
                ("Windows", "Win64", "Build.bat", "RunUAT.bat", "x64"),
                ("Linux", "Linux", "Linux/Build.sh", "RunUAT.sh", "x64")):
            for configuration in (None, "Development", "Shipping"):
                with self.subTest(system=system, package=configuration), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    engine = root / "Unreal Engine/Engine"
                    project = root / "Tests/Unreal/PostHogTestHost.uproject"
                    argv = ["build-test-host.py", "--ue-root", str(engine.parent)]
                    if configuration:
                        argv += ["--package", configuration]
                    with patch.object(build, "ROOT", root), patch.object(build, "PROJECT", project), \
                            patch.object(build.platform, "system", return_value=system), \
                            patch("sys.argv", argv), patch.object(build.subprocess, "run") as run:
                        build.main()
                    commands = [call.args[0] for call in run.call_args_list]
                    self.assertEqual(commands[0], [build.sys.executable, str(root / "scripts/copy-plugin.py"),
                                                   str(project.parent / "Plugins/PostHog")])
                    self.assertEqual(commands[1], [str(engine / "Build/BatchFiles" / build_script),
                                                   "PostHogTestHostEditor", target, "Development", str(project),
                                                   f"-architecture={architecture}", "-WaitMutex", "-NoUBA", "-NoHotReload"])
                    self.assertEqual(len(commands), 3 if configuration else 2)
                    if configuration:
                        self.assertEqual(commands[2][0], str(engine / "Build/BatchFiles" / uat_script))
                        for argument in ("BuildCookRun", f"-project={project}", f"-platform={target}",
                                         f"-clientconfig={configuration}", f"-specifiedarchitecture={architecture}",
                                         f"-archivedirectory={root / 'artifacts/packages' / target / configuration}"):
                            self.assertIn(argument, commands[2])
                    self.assertTrue(all(call.kwargs["check"] for call in run.call_args_list))

    def test_archive_cli_restores_checksum_pinned_zip(self):
        restore = load_script("restore-ci-archive")
        extractor = "tar" if os.name == "nt" else "unzip"
        if not shutil.which(extractor):
            self.skipTest(f"{extractor} is not installed")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as archive:
            archive.writestr("Engine/Build/version.txt", "5.8")
        data = buffer.getvalue()
        environment = {"CI_ARCHIVE_URL": "https://example.invalid/artifact.zip",
                       "CI_ARCHIVE_SHA256": hashlib.sha256(data).hexdigest()}
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "Unreal Engine"
            with patch.dict(os.environ, environment), patch("sys.argv", ["restore-ci-archive.py", str(destination)]), \
                    patch.object(restore.urllib.request, "urlopen", return_value=io.BytesIO(data)):
                restore.main()
            self.assertEqual((destination / "Engine/Build/version.txt").read_text(), "5.8")

    def test_archive_checksum_mismatch_does_not_extract(self):
        restore = load_script("restore-ci-archive")
        environment = {"CI_ARCHIVE_URL": "https://example.invalid/artifact.zip", "CI_ARCHIVE_SHA256": "0" * 64}
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "engine"
            with patch.dict(os.environ, environment), patch("sys.argv", ["restore-ci-archive.py", str(destination)]), \
                    patch.object(restore.urllib.request, "urlopen", return_value=io.BytesIO(b"bad archive")), \
                    patch.object(restore.subprocess, "run") as run:
                with self.assertRaisesRegex(SystemExit, "SHA-256 mismatch"):
                    restore.main()
            run.assert_not_called()
            self.assertFalse(destination.exists())

    def test_archive_download_error_redacts_url(self):
        restore = load_script("restore-ci-archive")
        url = "https://example.invalid/artifact.zip?token=fixture"
        environment = {"CI_ARCHIVE_URL": url, "CI_ARCHIVE_SHA256": "0" * 64}
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "engine"
            with patch.dict(os.environ, environment), patch("sys.argv", ["restore-ci-archive.py", str(destination)]), \
                    patch.object(restore.urllib.request, "urlopen", side_effect=OSError(url)):
                with self.assertRaises(SystemExit) as raised:
                    restore.main()
            self.assertNotIn(url, str(raised.exception))
            self.assertNotIn("token=", str(raised.exception))
            self.assertFalse(destination.exists())

    def test_archive_members_are_confined(self):
        restore = load_script("restore-ci-archive")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.zip"
            for name in ("../escape", "/absolute", "C:/drive", "folder\\escape"):
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr(name, "data")
                with self.assertRaises(ValueError):
                    restore.validate_archive(path)
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr("Engine/Build/version.txt", "5.8")
            restore.validate_archive(path)

    def test_archive_preserves_only_confined_symlinks(self):
        restore = load_script("restore-ci-archive")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "test.zip"
            entry = zipfile.ZipInfo("Engine/lib/link.so")
            entry.create_system = 3
            entry.external_attr = (stat.S_IFLNK | 0o777) << 16
            for target in ("../../../escape", "/outside", "C:/outside"):
                with zipfile.ZipFile(path, "w") as archive:
                    archive.writestr(entry, target)
                with self.assertRaises(ValueError):
                    restore.validate_archive(path)
            with zipfile.ZipFile(path, "w") as archive:
                archive.writestr(entry, "../other/library.so")
            restore.validate_archive(path)


class PluginCopyTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.sdk = self.root / "SDK"
        (self.sdk / "Source").mkdir(parents=True)
        (self.sdk / "Source/current.cpp").write_text("current source")
        (self.sdk / "PostHog.uplugin").write_text("{}")
        (self.sdk / "Tests/Unreal").mkdir(parents=True)
        (self.sdk / "Tests/Unreal/Nested.Build.cs").touch()
        self.destination = self.root / "Consumer/Plugins/PostHog"
        self.copy = load_script("copy-plugin")
        self.copy.ROOT = self.sdk

    def test_installs_only_plugin_source_and_descriptor(self):
        self.copy.copy_plugin(self.destination)
        self.assertEqual({path.name for path in self.destination.iterdir()},
                         {"Source", "PostHog.uplugin", ".posthog-source-copy"})
        self.assertEqual((self.destination / "Source/current.cpp").read_text(), "current source")

    def test_refresh_removes_stale_source_and_preserves_compiled_products(self):
        self.copy.copy_plugin(self.destination)
        (self.destination / "Source/stale.cpp").touch()
        (self.destination / "Binaries").mkdir()
        binary = self.destination / "Binaries/compiled"
        binary.write_bytes(b"compiled")
        (self.sdk / "Source/current.cpp").write_text("updated source")
        self.copy.copy_plugin(self.destination)
        self.assertFalse((self.destination / "Source/stale.cpp").exists())
        self.assertEqual((self.destination / "Source/current.cpp").read_text(), "updated source")
        self.assertEqual(binary.read_bytes(), b"compiled")

    def test_refresh_recovers_managed_copy_with_missing_source(self):
        self.destination.mkdir(parents=True)
        (self.destination / ".posthog-source-copy").touch()
        self.copy.copy_plugin(self.destination)
        self.assertEqual((self.destination / "Source/current.cpp").read_text(), "current source")

    def test_unmanaged_destination_is_preserved(self):
        self.destination.mkdir(parents=True)
        existing = self.destination / "custom.cpp"
        existing.write_text("user source")
        with self.assertRaisesRegex(RuntimeError, "unmanaged plugin"):
            self.copy.copy_plugin(self.destination)
        self.assertEqual(existing.read_text(), "user source")
        self.assertEqual(list(self.destination.iterdir()), [existing])


if __name__ == "__main__":
    unittest.main()
