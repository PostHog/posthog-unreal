#!/usr/bin/env python3
"""Update the PostHog Unreal SDK version across descriptor, Rust FFI crate, lockfile, and C++ identity."""
import argparse
import importlib.util
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def _load_packaging():
    spec = importlib.util.spec_from_file_location("package_plugin", ROOT / "scripts/package-plugin.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def replace_once(text, pattern, replacement, label):
    updated, count = re.subn(pattern, lambda m: m.group(1) + replacement + m.group(2), text, flags=re.MULTILINE)
    if count != 1:
        raise ValueError(f"Expected exactly one {label} declaration, found {count}")
    return updated


def bump_version(new_version, root=ROOT):
    pkg = _load_packaging()
    if not re.fullmatch(pkg.VERSION_PATTERN, new_version):
        raise ValueError(f"Invalid semver version (omit any v prefix): {new_version!r}")

    descriptor_path = root / "PostHog.uplugin"
    crate_path = root / "rust" / pkg.BRIDGE / "Cargo.toml"
    lock_path = root / "rust/Cargo.lock"
    client_path = root / "Source/PostHog/Private/PostHogClient.cpp"

    descriptor = descriptor_path.read_bytes().decode("utf-8")
    current = json.loads(descriptor)
    if current["VersionName"] != new_version:
        descriptor = replace_once(
            descriptor,
            r'^(\s*"VersionName": ")[^"\n]*(")',
            new_version,
            "PostHog.uplugin VersionName",
        )
        descriptor = replace_once(
            descriptor,
            r'^(\s*"Version": )\d+(,?)',
            str(current["Version"] + 1),
            "PostHog.uplugin Version",
        )

    crate = replace_once(
        crate_path.read_bytes().decode("utf-8"),
        r'^(version = ")[^"\n]*(")(?=\r?$)',
        new_version,
        f"rust/{pkg.BRIDGE}/Cargo.toml version",
    )
    lock = replace_once(
        lock_path.read_bytes().decode("utf-8"),
        rf'^(name = "{pkg.BRIDGE}"\r?\nversion = ")[^"\n]*(")(?=\r?$)',
        new_version,
        "rust/Cargo.lock bridge version",
    )
    client = replace_once(
        client_path.read_bytes().decode("utf-8"),
        r'(posthog_stateful_config_set_sdk_identity\([^,]+,\s*"posthog-unreal",\s*")[^"\n]*("\))',
        new_version,
        "PostHogClient.cpp sdk_version",
    )

    for path, content in (
        (descriptor_path, descriptor),
        (crate_path, crate),
        (lock_path, lock),
        (client_path, client),
    ):
        path.write_bytes(content.encode("utf-8"))

    verified = pkg.get_version(root)
    if verified != new_version:
        raise ValueError(f"Version verification failed: expected {new_version!r}, got {verified!r}")
    return verified


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("version", help="New semver version without v prefix, e.g. 0.0.2 or 0.1.0-alpha.1")
    args = parser.parse_args()
    print(bump_version(args.version))


if __name__ == "__main__":
    main()
