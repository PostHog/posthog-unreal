#!/usr/bin/env python3
"""Prepare and validate source releases of the Unreal plugin."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
PACKAGE = "posthog-unreal-ffi"
SEMVER = r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-[0-9A-Za-z.-]+)?"


def bridge_version(root):
    return tomllib.loads((root / "rust" / PACKAGE / "Cargo.toml").read_text(encoding="utf-8"))["package"]["version"]


def check(root, tag=None):
    version = bridge_version(root)
    if not re.fullmatch(SEMVER, version):
        raise ValueError(f"Unsupported release version: {version}")
    descriptor = json.loads((root / "PostHog.uplugin").read_text(encoding="utf-8"))
    if descriptor["VersionName"] != version:
        raise ValueError("PostHog.uplugin VersionName must match the Rust bridge version")
    if type(descriptor["Version"]) is not int or descriptor["Version"] < 1:
        raise ValueError("PostHog.uplugin Version must be a positive integer")
    lock = tomllib.loads((root / "rust/Cargo.lock").read_text(encoding="utf-8"))
    locked = next(p for p in lock["package"] if p["name"] == PACKAGE)
    if locked["version"] != version:
        raise ValueError("Cargo.lock must match the Rust bridge version")
    if tag is not None and tag != f"v{version}":
        raise ValueError(f"Release tag must be v{version}")
    if list((root / "rust/.sampo/changesets").glob("*.md")):
        raise ValueError("Consume pending changesets before releasing")
    return version, descriptor


def prepare(root):
    # Release preparation usually happens on a short-lived PR branch.
    env = {**os.environ, "SAMPO_RELEASE_BRANCH": "main"}
    result = subprocess.run(["sampo", "release"], cwd=root / "rust", env=env)
    if result.returncode != 2:
        result.check_returncode()
    path = root / "PostHog.uplugin"
    descriptor = json.loads(path.read_text(encoding="utf-8"))
    version = bridge_version(root)
    if descriptor["VersionName"] != version:
        descriptor["VersionName"] = version
        descriptor["Version"] += 1
        path.write_text(json.dumps(descriptor, indent=2) + "\n", encoding="utf-8")
    check(root)


def release_notes(root, version):
    changelog = (root / "rust" / PACKAGE / "CHANGELOG.md").read_text(encoding="utf-8")
    sections = re.split(r"(?m)^## ", changelog)[1:]
    for section in sections:
        heading, _, body = section.partition("\n")
        if heading.split()[0] == version:
            return body.strip() + "\n"
    raise ValueError(f"No changelog entry for {version}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["prepare", "check", "notes"])
    parser.add_argument("--tag")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.command == "prepare":
        prepare(ROOT)
        return
    version, descriptor = check(ROOT, args.tag)
    if args.command == "notes":
        if args.output is None:
            parser.error("notes requires --output")
        args.output.write_text(release_notes(ROOT, version), encoding="utf-8")
    print(json.dumps({"version": version, "prerelease": bool(descriptor.get("IsBetaVersion")) or "-" in version}))


if __name__ == "__main__":
    main()
