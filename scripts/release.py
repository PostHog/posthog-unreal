#!/usr/bin/env python3
"""Validate the SDK version and prepare checksummed release assets locally; never publish."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
BRIDGE = "posthog-unreal-ffi"
VERSION_PATTERN = r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?"
# Every release bundles all three native libraries; there are no partial-platform releases.
TARGETS = {
    "aarch64-apple-darwin": "libposthog_unreal_ffi.a",
    "x86_64-unknown-linux-gnu": "libposthog_unreal_ffi.a",
    "x86_64-pc-windows-msvc": "posthog_unreal_ffi.lib",
}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def get_version(root=ROOT):
    """Return the pnpm release version after checking every declaration agrees with it."""
    version = json.loads((root / "package.json").read_text(encoding="utf-8"))["version"]
    if not re.fullmatch(VERSION_PATTERN, version):
        raise ValueError(f"Invalid release version in package.json: {version!r}")
    descriptor = json.loads((root / "PostHog.uplugin").read_text(encoding="utf-8"))
    if type(descriptor.get("Version")) is not int or descriptor["Version"] < 1:
        raise ValueError("PostHog.uplugin Version must be a positive integer")
    crate = tomllib.loads((root / "rust" / BRIDGE / "Cargo.toml").read_text(encoding="utf-8"))
    lock = tomllib.loads((root / "rust/Cargo.lock").read_text(encoding="utf-8"))
    locked = [package["version"] for package in lock["package"] if package["name"] == BRIDGE]
    declarations = {
        "PostHog.uplugin VersionName": descriptor.get("VersionName"),
        f"rust/{BRIDGE}/Cargo.toml": crate["package"]["version"],
        "rust/Cargo.lock": locked[0] if len(locked) == 1 else None,
    }
    for name, declared in declarations.items():
        if declared != version:
            raise ValueError(
                f"{name} declares {declared!r}, but package.json declares {version!r}; "
                "run scripts/bump_version.py")
    return version


def validate_tag(tag, root=ROOT):
    version = get_version(root)
    if tag != version:
        raise ValueError(f"Tag {tag!r} must match the SDK version {version!r} without a v prefix")
    return version


def validate_archive(archive, version):
    """Check the plugin ZIP is the complete three-platform plugin for this version."""
    with zipfile.ZipFile(archive) as package:
        if package.testzip() is not None:
            raise ValueError(f"Corrupt plugin archive: {archive.name}")
        names = set(package.namelist())
        outside = sorted(name for name in names if not name.startswith("PostHog/"))
        if outside:
            raise ValueError(f"Plugin archive has files outside PostHog/: {outside[:3]}")

        def read(name):
            if "PostHog/" + name not in names:
                raise ValueError(f"Plugin archive is missing {name}")
            return package.read("PostHog/" + name)

        if json.loads(read("PostHog.uplugin"))["VersionName"] != version:
            raise ValueError(f"Plugin archive descriptor does not declare {version}")
        for target, library in TARGETS.items():
            prefix = f"Source/ThirdParty/PostHogRust/lib/{target}/"
            manifest = json.loads(read(prefix + "manifest.json"))
            expected = {"target": target, "bridge_version": version, "sha256": sha256(read(prefix + library))}
            for key, value in expected.items():
                if manifest.get(key) != value:
                    raise ValueError(f"Plugin archive native manifest mismatch for {target}: {key}")
            if not read(f"ThirdPartyNotices/{target}.txt").strip():
                raise ValueError(f"Plugin archive has empty notices for {target}")


def prepare_release(tag, directory, root=ROOT):
    version = validate_tag(tag, root)
    archive = directory / f"posthog-unreal-{version}.zip"
    if not archive.is_file():
        raise FileNotFoundError(f"Missing plugin archive: {archive}")
    validate_archive(archive, version)
    (directory / "SHA256SUMS").write_text(f"{sha256(archive.read_bytes())}  {archive.name}\n", encoding="utf-8")
    return archive


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tag", help="Version tag without a v prefix, e.g. 0.1.0")
    parser.add_argument("--check", action="store_true", help="Only validate the tag/version")
    parser.add_argument("--directory", type=Path, default=ROOT / "artifacts/releases")
    args = parser.parse_args()
    validate_tag(args.tag)
    if not args.check:
        prepare_release(args.tag, args.directory)
        print(f"Prepared release assets in {args.directory}")


if __name__ == "__main__":
    main()
