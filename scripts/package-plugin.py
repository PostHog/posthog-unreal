#!/usr/bin/env python3
"""Stage native build outputs, then combine them into one installable Unreal plugin."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import zipfile

from release import ROOT, TARGETS, get_version


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def validate_native(root, target, files):
    version = get_version(root)
    prefix = f"Source/ThirdParty/PostHogRust/lib/{target}/"
    manifest = json.loads(files[prefix + "manifest.json"])
    header = (root / "Source/ThirdParty/PostHogRust/include/posthog_unreal.h").read_text(encoding="utf-8")
    abi = int(re.search(r"#define PHU_ABI_VERSION (\d+)u", header)[1])
    expected = {
        "target": target,
        "bridge_version": version,
        "abi_version": abi,
        "sha256": hashlib.sha256(files[prefix + TARGETS[target]]).hexdigest(),
        "lockfile_sha256": sha256(root / "rust/Cargo.lock"),
    }
    for key, value in expected.items():
        if manifest.get(key) != value:
            raise ValueError(f"Native manifest mismatch for {target}: {key}; rebuild the library")


def stage_native(root, target, output, notices, metadata, rust_docs):
    native = root / "Source/ThirdParty/PostHogRust/lib" / target
    files = {}
    for path in (native / TARGETS[target], native / "manifest.json"):
        files[path.relative_to(root).as_posix()] = path.read_bytes()
    validate_native(root, target, files)
    notices_data = notices.read_bytes()
    if not notices_data.decode("utf-8").strip():
        raise ValueError("Third-party notices must not be empty")
    files[f"ThirdPartyNotices/{target}.txt"] = notices_data

    # Rust's bundled standard library also carries redistribution notices.
    copyrights = sorted(rust_docs.glob("COPYRIGHT*.html"))
    licenses = sorted((rust_docs / "licenses").glob("*.txt"))
    if not copyrights or not licenses:
        raise ValueError(f"Rust toolchain has no copyright notices under {rust_docs}")
    for path in [*copyrights, *licenses]:
        files[f"RustNotices/{target}/" + path.relative_to(rust_docs).as_posix()] = path.read_bytes()

    # MPL dependencies require their corresponding source to remain available to recipients.
    nodes = {node["id"] for node in metadata["resolve"]["nodes"]}
    for dependency in metadata["packages"]:
        if dependency["id"] in nodes and "MPL-2.0" in (dependency.get("license") or ""):
            source = Path(dependency["manifest_path"]).parent
            prefix = f'ThirdPartySources/{target}/{dependency["name"]}-{dependency["version"]}/'
            for path in sorted(source.rglob("*")):
                if path.is_file() and not path.is_symlink():
                    files[prefix + path.relative_to(source).as_posix()] = path.read_bytes()

    destination = output / target
    # Native bundles are immutable inputs to assembly; stage into a fresh directory.
    destination.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        path = destination / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data)
    return destination


def package_plugin(root, native_root, output):
    version = get_version(root)
    files = {}
    for target, library in TARGETS.items():
        bundle = native_root / target
        if not bundle.is_dir():
            raise ValueError(f"Missing native bundle: {target}")
        prefix = f"Source/ThirdParty/PostHogRust/lib/{target}/"
        allowed_files = {prefix + library, prefix + "manifest.json", f"ThirdPartyNotices/{target}.txt"}
        allowed_directories = (f"RustNotices/{target}/", f"ThirdPartySources/{target}/")
        native_files = {}
        for path in sorted(bundle.rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Native bundle contains a symlink: {path}")
            if path.is_file():
                name = path.relative_to(bundle).as_posix()
                if name not in allowed_files and not name.startswith(allowed_directories):
                    raise ValueError(f"Unexpected native bundle file: {name}")
                native_files[name] = path.read_bytes()
        for name in allowed_files:
            if name not in native_files:
                raise ValueError(f"Native bundle is missing {name}")
        if not native_files[f"ThirdPartyNotices/{target}.txt"].decode("utf-8").strip():
            raise ValueError(f"Native bundle has empty notices: {target}")
        if not list((bundle / f"RustNotices/{target}").glob("COPYRIGHT*.html")) or not list(
                (bundle / f"RustNotices/{target}/licenses").glob("*.txt")):
            raise ValueError(f"Native bundle is missing Rust copyright notices: {target}")
        validate_native(root, target, native_files)
        files.update(native_files)

    for module in ("PostHog", "PostHogAnalytics"):
        for path in sorted((root / "Source" / module).rglob("*")):
            if path.is_file() and path.suffix in {".h", ".cpp", ".cs", ".inl"}:
                files[path.relative_to(root).as_posix()] = path.read_bytes()
    third_party = root / "Source/ThirdParty/PostHogRust"
    for path in sorted((third_party / "include").rglob("*.h")):
        files[path.relative_to(root).as_posix()] = path.read_bytes()
    for path in (third_party / "PostHogRust.Build.cs", root / "PostHog.uplugin", root / "LICENSE"):
        files[path.relative_to(root).as_posix()] = path.read_bytes()
    files["CHANGELOG.md"] = (root / "CHANGELOG.md").read_bytes()
    files["THIRD_PARTY_NOTICES.txt"] = (
        "PostHog Unreal SDK dependency notices, grouped by native build target:\n\n" +
        "".join(f"- ThirdPartyNotices/{target}.txt\n" for target in TARGETS) +
        "\nRust standard-library notices are under RustNotices/<target>/.\n"
        "Corresponding MPL dependency sources are under ThirdPartySources/<target>/.\n"
    ).encode()
    files["README.md"] = f"""# PostHog Unreal SDK {version}

Development SDK; not production-ready. Targets UE 5.8:
- macOS 14+ on Apple Silicon (arm64, single architecture)
- Linux x64 (native library built on Ubuntu 22.04)
- Windows x64 (native library built with MSVC on Windows Server 2022)

Copy the PostHog folder into <Project>/Plugins/ and rebuild your C++ project.
The matching Rust static libraries are bundled; you do not need Rust or Cargo.
Unreal links only the library for your target platform.
A Blueprint-only project must first add a C++ class and use the Unreal C++ toolchain.

Setup, API, screenshot privacy guidance and platform validation status:
https://github.com/PostHog/posthog-unreal/blob/{version}/README.md

See LICENSE, THIRD_PARTY_NOTICES.txt, RustNotices/ and ThirdPartySources/ for
license texts, copyright notices and corresponding dependency sources.
""".encode()

    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"posthog-unreal-{version}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as package:
        for name, data in sorted(files.items()):
            # Stable ZIP metadata makes identical inputs produce the same checksum.
            info = zipfile.ZipInfo("PostHog/" + name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            package.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    stage = commands.add_parser("stage", help="Collect one native runner's library and notices")
    stage.add_argument("target", choices=TARGETS)
    stage.add_argument("--notices", type=Path, required=True)
    stage.add_argument("--output", type=Path, default=ROOT / "artifacts/native")
    package = commands.add_parser("package", help="Assemble all native bundles with the plugin source")
    package.add_argument("--native", type=Path, default=ROOT / "artifacts/native")
    package.add_argument("--output", type=Path, default=ROOT / "artifacts/releases")
    args = parser.parse_args()
    if args.command == "stage":
        metadata = json.loads(subprocess.check_output([
            "cargo", "metadata", "--manifest-path", str(ROOT / "rust/Cargo.toml"),
            "--locked", "--filter-platform", args.target, "--format-version", "1",
        ], text=True))
        sysroot = Path(subprocess.check_output(["rustc", "--print", "sysroot"], text=True).strip())
        print(stage_native(ROOT, args.target, args.output, args.notices, metadata, sysroot / "share/doc/rust"))
    else:
        print(package_plugin(ROOT, args.native, args.output))


if __name__ == "__main__":
    main()
