#!/usr/bin/env python3
"""Copy the pnpm release version to the plugin descriptor and the Rust bridge crate."""
import json
import re

import release


def replace_once(text, pattern, replacement, name):
    updated, count = re.subn(pattern, lambda match: match[1] + replacement, text, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"Expected one {name} declaration")
    return updated


def bump_version(root=release.ROOT):
    version = json.loads((root / "package.json").read_text(encoding="utf-8"))["version"]
    if not re.fullmatch(release.VERSION_PATTERN, version):
        raise ValueError(f"Invalid release version: {version!r}")
    descriptor_path = root / "PostHog.uplugin"
    crate_path = root / "rust" / release.BRIDGE / "Cargo.toml"
    lock_path = root / "rust/Cargo.lock"
    descriptor = descriptor_path.read_bytes().decode("utf-8")
    current = json.loads(descriptor)
    if current["VersionName"] != version:
        # Unreal compares the integer Version to order plugin updates; VersionName is for display.
        descriptor = replace_once(descriptor, r'^(\s*"VersionName": )"[^"\n]*"', f'"{version}"',
                                  "PostHog.uplugin VersionName")
        descriptor = replace_once(descriptor, r'^(\s*"Version": )\d+', str(current["Version"] + 1),
                                  "PostHog.uplugin Version")
    # The bridge reports its crate version as $lib_version, so it must equal the plugin version.
    crate = replace_once(crate_path.read_bytes().decode("utf-8"), r'^(version = )"[^"\n]*"$', f'"{version}"',
                         f"rust/{release.BRIDGE}/Cargo.toml version")
    lock = replace_once(lock_path.read_bytes().decode("utf-8"),
                        rf'^(name = "{release.BRIDGE}"\nversion = )"[^"\n]*"$', f'"{version}"',
                        "rust/Cargo.lock bridge version")
    # Validate every file before writing any, so a failure never leaves a partial bump. Bytes keep
    # the checked-out line endings on every platform.
    for path, content in ((descriptor_path, descriptor), (crate_path, crate), (lock_path, lock)):
        path.write_bytes(content.encode("utf-8"))
    return version


if __name__ == "__main__":
    version = bump_version()
    if release.get_version() != version:
        raise SystemExit(f"Version declarations do not match {version}")
    print(version)
