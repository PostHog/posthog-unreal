#!/usr/bin/env python3
"""Build the Rust bridge and install the target's static library into the plugin."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tomllib

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = {
    "aarch64-apple-darwin": "libposthog_unreal_ffi.a",
    "x86_64-apple-darwin": "libposthog_unreal_ffi.a",
    "x86_64-unknown-linux-gnu": "libposthog_unreal_ffi.a",
    "x86_64-pc-windows-msvc": "posthog_unreal_ffi.lib",
}


def cargo_patch(core):
    return 'patch."ssh://git@github.com/PostHog/posthog-rs.git".posthog.path=' + json.dumps(str(core))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", choices=LIBRARIES)
    parser.add_argument("--test", action="store_true", help="Also run bridge unit tests on the host target")
    args = parser.parse_args()
    # The immutable Cargo dependency is the default. A local override is for development only.
    override = os.environ.get("POSTHOG_RS_PATH")
    config = ["--config", cargo_patch(Path(override).resolve())] if override else []
    env = {**os.environ, "CARGO_NET_GIT_FETCH_WITH_CLI": "true"}
    if args.target.endswith("apple-darwin"):
        env.setdefault("MACOSX_DEPLOYMENT_TARGET", "14.0")
    manifest_path = ["--manifest-path", str(ROOT / "rust/Cargo.toml")]
    metadata = json.loads(subprocess.check_output(
        ["cargo", "metadata", *config, *manifest_path, "--locked", "--format-version", "1"], env=env, text=True))
    dependency = next(p for p in metadata["packages"] if p["name"] == "posthog")
    core = Path(dependency["manifest_path"]).parent
    common = [*config, *manifest_path, "--locked", "--release", "--target", args.target]
    subprocess.run(["cargo", "build", *common], env=env, check=True)
    if args.test:
        subprocess.run(["cargo", "test", *common], env=env, check=True)
    target_dir = Path(env.get("CARGO_TARGET_DIR", ROOT / "rust/target")).resolve()
    destination = ROOT / "Source/ThirdParty/PostHogRust/lib" / args.target
    destination.mkdir(parents=True, exist_ok=True)
    library = destination / LIBRARIES[args.target]
    shutil.copy2(target_dir / args.target / "release" / library.name, library)
    lock = (ROOT / "rust/Cargo.lock").read_bytes()
    locked_core = next(p for p in tomllib.loads(lock.decode())["package"] if p["name"] == "posthog")
    source = locked_core.get("source", "local development override")
    core_base = source.rsplit("#", 1)[-1] if "#" in source else subprocess.check_output(
        ["git", "-C", str(core), "rev-parse", "HEAD"], text=True).strip()
    source_hash = hashlib.sha256()
    for path in sorted([core / "Cargo.toml", core / "Cargo.lock", *(core / "src").rglob("*.rs")]):
        source_hash.update(str(path.relative_to(core)).replace("\\", "/").encode() + b"\0")
        source_hash.update(path.read_bytes() + b"\0")
    manifest = {
        "abi_version": 3, "bridge_version": "0.0.1", "core_package": "posthog", "core_version": locked_core["version"],
        "core_source": source, "development_override": bool(override), "target": args.target,
        "core_base": core_base,
        "core_source_sha256": source_hash.hexdigest(),
        "rustc": subprocess.check_output(["rustc", "--version"], text=True).strip(),
        "sha256": hashlib.sha256(library.read_bytes()).hexdigest(),
        "lockfile_sha256": hashlib.sha256(lock).hexdigest(), "core_default_features": False,
        "core_features": ["tls", "session-replay"], "ingestion": "v1", "analytics_endpoint": "/i/v1/analytics/events",
    }
    (destination / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
