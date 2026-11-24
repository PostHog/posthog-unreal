#!/usr/bin/env python3
"""Build posthog-stateful-ffi and copy the static library into the Unreal plugin."""
import argparse
import os
from pathlib import Path
import platform
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
LIBRARIES = {
    "aarch64-apple-darwin": "libposthog_stateful_ffi.a",
    "x86_64-unknown-linux-gnu": "libposthog_stateful_ffi.a",
    "x86_64-pc-windows-msvc": "posthog_stateful_ffi.lib",
}


def host_target():
    system = platform.system()
    machine = platform.machine().lower()
    if system == "Darwin" and machine in ("arm64", "aarch64"):
        return "aarch64-apple-darwin"
    if system == "Linux" and machine in ("x86_64", "amd64"):
        return "x86_64-unknown-linux-gnu"
    if system == "Windows" and machine in ("x86_64", "amd64"):
        return "x86_64-pc-windows-msvc"
    raise RuntimeError(f"Unsupported host platform: {system} {machine}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("target", nargs="?", default=None, choices=LIBRARIES)
    parser.add_argument("--test", action="store_true", help="Also run Rust FFI tests on the host target")
    args = parser.parse_args()
    target = args.target or host_target()

    env = os.environ.copy()
    if target.endswith("apple-darwin"):
        env.setdefault("MACOSX_DEPLOYMENT_TARGET", "14.0")

    manifest = str(ROOT / "rust/Cargo.toml")
    if args.test:
        subprocess.run(["cargo", "test", "--manifest-path", manifest, "--locked"], env=env, check=True)
    subprocess.run(
        ["cargo", "build", "--manifest-path", manifest, "--locked", "--release", "--target", target],
        env=env,
        check=True,
    )

    target_dir = Path(env.get("CARGO_TARGET_DIR", ROOT / "rust/target")).resolve()
    lib_name = LIBRARIES[target]
    src = target_dir / target / "release" / lib_name
    dst_dir = ROOT / "Source/ThirdParty/PostHogRust/lib" / target
    dst_dir.mkdir(parents=True, exist_ok=True)
    dst = dst_dir / lib_name
    shutil.copy2(src, dst)
    print(f"Installed {dst}")


if __name__ == "__main__":
    main()
