#!/usr/bin/env python3
"""Stage per-platform native libraries and package a drop-in Unreal plugin ZIP."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = "posthog-stateful-ffi"
VERSION_PATTERN = (
    r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)"
    r"(?:-(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[A-Za-z-][0-9A-Za-z-]*))*)?"
)
RUST_STD_LICENSES = ("Apache-2.0.txt", "MIT.txt")

# Every release bundles all three desktop native libraries.
TARGETS = {
    "aarch64-apple-darwin": "libposthog_stateful_ffi.a",
    "x86_64-unknown-linux-gnu": "libposthog_stateful_ffi.a",
    "x86_64-pc-windows-msvc": "posthog_stateful_ffi.lib",
}


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def _cargo_package_version(text):
    section = re.search(r"(?ms)^\[package\]\s*\n(.*?)(?=^\[|\Z)", text)
    if not section:
        return None
    matches = re.findall(r'(?m)^version\s*=\s*"([^"\n]+)"\s*$', section.group(1))
    return matches[0] if len(matches) == 1 else None


def _cargo_lock_versions(text, package_name):
    versions = []
    for block in re.split(r"(?m)^\[\[package\]\]\s*$", text)[1:]:
        body = re.split(r"(?m)^\[", block, maxsplit=1)[0]
        if re.search(rf'(?m)^name\s*=\s*"{re.escape(package_name)}"\s*$', body):
            match = re.search(r'(?m)^version\s*=\s*"([^"\n]+)"\s*$', body)
            if match:
                versions.append(match.group(1))
    return versions


def get_version(root=ROOT):
    """Return the SDK version after verifying every declaration matches."""
    descriptor = json.loads((root / "PostHog.uplugin").read_text(encoding="utf-8"))
    version = descriptor.get("VersionName")
    if not isinstance(version, str) or not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError(f"Invalid VersionName in PostHog.uplugin: {version!r}")
    if type(descriptor.get("Version")) is not int or descriptor["Version"] < 1:
        raise ValueError("PostHog.uplugin Version must be a positive integer")

    crate_version = _cargo_package_version((root / "rust" / BRIDGE / "Cargo.toml").read_text(encoding="utf-8"))
    locked = _cargo_lock_versions((root / "rust/Cargo.lock").read_text(encoding="utf-8"), BRIDGE)

    client_cpp = (root / "Source/PostHog/Private/PostHogClient.cpp").read_text(encoding="utf-8")
    sdk_match = re.search(
        r'posthog_stateful_config_set_sdk_identity\([^,]+,\s*"posthog-unreal",\s*"([^"]+)"\)',
        client_cpp,
    )
    sdk_version = sdk_match.group(1) if sdk_match else None

    declarations = {
        f"rust/{BRIDGE}/Cargo.toml": crate_version,
        "rust/Cargo.lock": locked[0] if len(locked) == 1 else None,
        "Source/PostHog/Private/PostHogClient.cpp": sdk_version,
    }
    for name, declared in declarations.items():
        if declared != version:
            raise ValueError(
                f"{name} declares {declared!r}, expected {version!r} (from PostHog.uplugin); "
                "run python3 scripts/bump-version.py"
            )
    return version


def validate_tag(tag, root=ROOT):
    version = get_version(root)
    if tag != version:
        raise ValueError(f"Tag {tag!r} must match SDK version {version!r} without a v prefix")
    return version


def _runtime_package_ids(metadata):
    nodes = {node["id"]: node for node in metadata["resolve"]["nodes"]}
    roots = [
        pkg["id"]
        for pkg in metadata["packages"]
        if pkg["name"] == BRIDGE and pkg["id"] in nodes
    ]
    if not roots:
        return set(nodes)
    visited = set()
    queue = list(roots)
    while queue:
        pkg_id = queue.pop()
        if pkg_id in visited or pkg_id not in nodes:
            continue
        visited.add(pkg_id)
        for dep in nodes[pkg_id].get("deps", []):
            kinds = dep.get("dep_kinds")
            if not kinds or any(entry.get("kind") is None for entry in kinds):
                queue.append(dep["pkg"])
    return visited


def stage_native(root, target, output, notices, metadata, rust_docs):
    """Stage one platform's static library, third-party notices, Rust notices, and MPL sources."""
    library = TARGETS[target]
    lib_path = root / "Source/ThirdParty/PostHogRust/lib" / target / library
    if not lib_path.is_file():
        raise FileNotFoundError(f"Missing built native library: {lib_path}")

    notices_data = notices.read_bytes()
    if not notices_data.decode("utf-8").strip():
        raise ValueError("Third-party notices file must not be empty")

    files = {
        f"Source/ThirdParty/PostHogRust/lib/{target}/{library}": lib_path.read_bytes(),
        f"ThirdPartyNotices/{target}.txt": notices_data,
    }

    # Include Rust's standard-library copyright and dual-license notices (not toolchain/GPL docs).
    lib_copyright = rust_docs / "COPYRIGHT-library.html"
    copyrights = [lib_copyright] if lib_copyright.is_file() else sorted(p for p in rust_docs.glob("COPYRIGHT*") if p.is_file())
    licenses = [rust_docs / "licenses" / name for name in RUST_STD_LICENSES if (rust_docs / "licenses" / name).is_file()]
    if not copyrights or len(licenses) != len(RUST_STD_LICENSES):
        raise ValueError(f"Rust toolchain is missing standard-library notices under {rust_docs}")
    for path in [*copyrights, *licenses]:
        files[f"RustNotices/{target}/" + path.relative_to(rust_docs).as_posix()] = path.read_bytes()

    # MPL-2.0 runtime dependencies require corresponding source to accompany distributed binaries.
    active_ids = _runtime_package_ids(metadata)
    for package in metadata["packages"]:
        if package["id"] in active_ids and "MPL-2.0" in (package.get("license") or ""):
            source = Path(package["manifest_path"]).parent
            prefix = f'ThirdPartySources/{target}/{package["name"]}-{package["version"]}/'
            for path in sorted(source.rglob("*")):
                rel = path.relative_to(source)
                if path.is_file() and not path.is_symlink() and ".git" not in rel.parts and path.name != ".cargo-ok":
                    files[prefix + rel.as_posix()] = path.read_bytes()

    destination = output / target
    if destination.exists():
        shutil.rmtree(destination)
    destination.mkdir(parents=True)
    for rel_path, data in files.items():
        out_path = destination / rel_path
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_bytes(data)
    return destination


def validate_archive(archive, version):
    """Verify the generated ZIP is a complete, drop-in three-platform PostHog/ plugin."""
    with zipfile.ZipFile(archive) as package:
        if package.testzip() is not None:
            raise ValueError(f"Corrupt plugin archive: {archive.name}")
        names = set(package.namelist())
        outside = sorted(name for name in names if not name.startswith("PostHog/"))
        if outside:
            raise ValueError(f"Plugin archive has entries outside PostHog/: {outside[:3]}")

        def read(rel):
            full = "PostHog/" + rel
            if full not in names:
                raise ValueError(f"Plugin archive is missing {rel}")
            return package.read(full)

        if json.loads(read("PostHog.uplugin"))["VersionName"] != version:
            raise ValueError(f"Plugin archive PostHog.uplugin does not declare {version}")
        read("LICENSE")
        read("README.md")
        read("THIRD_PARTY_NOTICES.txt")
        read("Source/PostHog/PostHog.Build.cs")
        read("Source/PostHogAnalytics/PostHogAnalytics.Build.cs")
        read("Source/ThirdParty/PostHogRust/PostHogRust.Build.cs")
        read("Source/ThirdParty/PostHogRust/include/posthog_stateful.h")

        for target, library in TARGETS.items():
            if not read(f"Source/ThirdParty/PostHogRust/lib/{target}/{library}"):
                raise ValueError(f"Plugin archive has empty native library for {target}")
            if not read(f"ThirdPartyNotices/{target}.txt").decode("utf-8").strip():
                raise ValueError(f"Plugin archive has empty notices for {target}")
            rust_prefix = f"PostHog/RustNotices/{target}/"
            if not any(n.startswith(rust_prefix + "COPYRIGHT") for n in names):
                raise ValueError(f"Plugin archive is missing Rust copyright notices for {target}")
            for license_name in RUST_STD_LICENSES:
                if not read(f"RustNotices/{target}/licenses/{license_name}").strip():
                    raise ValueError(f"Plugin archive has empty Rust license notice {license_name} for {target}")


def package_plugin(root, native_root, output):
    """Combine all three staged native bundles and plugin sources into a drop-in ZIP + SHA256SUMS."""
    version = get_version(root)
    files = {}

    for target, library in TARGETS.items():
        bundle = native_root / target
        if not bundle.is_dir():
            raise ValueError(f"Missing native bundle directory: {target}")
        allowed_files = {
            f"Source/ThirdParty/PostHogRust/lib/{target}/{library}",
            f"ThirdPartyNotices/{target}.txt",
        }
        allowed_prefixes = (f"RustNotices/{target}/", f"ThirdPartySources/{target}/")
        native_files = {}
        for path in sorted(bundle.rglob("*")):
            if path.is_symlink():
                raise ValueError(f"Native bundle contains a symlink: {path}")
            if path.is_file():
                rel = path.relative_to(bundle).as_posix()
                if rel not in allowed_files and not rel.startswith(allowed_prefixes):
                    raise ValueError(f"Unexpected file in native bundle: {rel}")
                native_files[rel] = path.read_bytes()
        for required in allowed_files:
            if required not in native_files:
                raise ValueError(f"Native bundle for {target} is missing {required}")
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

    files["THIRD_PARTY_NOTICES.txt"] = (
        "PostHog Unreal SDK third-party notices, grouped by native target:\n\n"
        + "".join(f"- ThirdPartyNotices/{target}.txt\n" for target in TARGETS)
        + "\nRust standard-library notices are under RustNotices/<target>/.\n"
        "Corresponding MPL-2.0 dependency sources are under ThirdPartySources/<target>/.\n"
    ).encode("utf-8")

    release_label = "Pre-release Unreal Engine 5.8 plugin" if (version.startswith("0.") or "-" in version) else "Unreal Engine 5.8 plugin"
    files["README.md"] = (
        f"# PostHog Unreal SDK {version}\n\n"
        f"{release_label} (C++ projects).\n"
        "Bundled prebuilt native static libraries (no Rust toolchain required):\n"
        "- macOS 14+ Apple Silicon (`aarch64-apple-darwin`)\n"
        "- Linux x64 (`x86_64-unknown-linux-gnu`)\n"
        "- Windows x64 (`x86_64-pc-windows-msvc`)\n\n"
        "## Install\n\n"
        "1. Extract this archive and copy the `PostHog/` folder into `<YourProject>/Plugins/PostHog/`.\n"
        "2. Regenerate project files and rebuild your C++ project.\n"
        "3. Configure your project token under Project Settings -> Plugins -> PostHog.\n\n"
        "A Blueprint-only project must first add a C++ class so Unreal can compile and link the plugin.\n\n"
        f"Documentation & API details: https://github.com/PostHog/posthog-unreal/blob/{version}/README.md\n\n"
        "See `LICENSE`, `THIRD_PARTY_NOTICES.txt`, `RustNotices/`, and `ThirdPartySources/` for license details.\n"
    ).encode("utf-8")

    output.mkdir(parents=True, exist_ok=True)
    archive = output / f"posthog-unreal-{version}.zip"
    with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_DEFLATED) as package:
        for name, data in sorted(files.items()):
            info = zipfile.ZipInfo("PostHog/" + name, date_time=(1980, 1, 1, 0, 0, 0))
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            package.writestr(info, data, compress_type=zipfile.ZIP_DEFLATED)

    validate_archive(archive, version)
    checksums = output / "SHA256SUMS"
    checksums.write_text(f"{sha256_bytes(archive.read_bytes())}  {archive.name}\n", encoding="utf-8")
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    check = sub.add_parser("check-version", help="Verify all SDK version declarations match")
    check.add_argument("--tag", help="Optional release tag (without v prefix) that must match")

    stage = sub.add_parser("stage", help="Stage one target's native library and notices")
    stage.add_argument("target", choices=TARGETS)
    stage.add_argument("--notices", type=Path, required=True)
    stage.add_argument("--output", type=Path, default=ROOT / "artifacts/native")

    pkg = sub.add_parser("package", help="Assemble the drop-in plugin ZIP and SHA256SUMS")
    pkg.add_argument("--native", type=Path, default=ROOT / "artifacts/native")
    pkg.add_argument("--output", type=Path, default=ROOT / "artifacts/releases")

    args = parser.parse_args()
    if args.command == "check-version":
        version = validate_tag(args.tag) if args.tag else get_version()
        print(version)
    elif args.command == "stage":
        metadata = json.loads(
            subprocess.check_output(
                [
                    "cargo",
                    "metadata",
                    "--manifest-path",
                    str(ROOT / "rust/Cargo.toml"),
                    "--locked",
                    "--filter-platform",
                    args.target,
                    "--format-version",
                    "1",
                ],
                encoding="utf-8",
            )
        )
        sysroot = Path(subprocess.check_output(["rustc", "--print", "sysroot"], encoding="utf-8").strip())
        print(stage_native(ROOT, args.target, args.output, args.notices, metadata, sysroot / "share/doc/rust"))
    else:
        print(package_plugin(ROOT, args.native, args.output))


if __name__ == "__main__":
    main()
