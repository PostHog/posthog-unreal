#!/usr/bin/env python3
"""Build the minimal SDK consumer with an installed UE 5.8 engine."""
import argparse
import os
from pathlib import Path
import platform
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
PROJECT = ROOT / "Tests/Unreal/PostHogTestHost.uproject"


def host_platform():
    return {"Darwin": "Mac", "Windows": "Win64", "Linux": "Linux"}[platform.system()]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ue-root", default=os.environ.get("UE_ROOT"), required=not os.environ.get("UE_ROOT"))
    parser.add_argument("--package", choices=["Development", "Shipping"])
    args = parser.parse_args()
    engine = Path(args.ue_root).resolve() / "Engine"
    target = host_platform()
    build = engine / "Build/BatchFiles" / ({"Mac": "Mac/Build.sh", "Linux": "Linux/Build.sh", "Win64": "Build.bat"}[target])
    subprocess.run([sys.executable, str(ROOT / "scripts/copy-plugin.py"), str(PROJECT.parent / "Plugins/PostHog")], check=True)
    architecture = ["-architecture=arm64"] if target == "Mac" else ["-architecture=x64"]
    subprocess.run([str(build), "PostHogTestHostEditor", target, "Development", str(PROJECT), *architecture,
                    "-WaitMutex", "-NoUBA", "-NoHotReload"], check=True)
    if args.package:
        uat = engine / "Build/BatchFiles" / ("RunUAT.bat" if target == "Win64" else "RunUAT.sh")
        subprocess.run([str(uat), "BuildCookRun", f"-project={PROJECT}", f"-platform={target}",
                        f"-clientconfig={args.package}", "-specifiedarchitecture=" + ("arm64" if target == "Mac" else "x64"),
                        "-build", "-cook", "-stage", "-pak", "-package", "-archive",
                        f"-archivedirectory={ROOT / 'artifacts/packages' / target / args.package}",
                        "-unattended", "-utf8output", "-nop4", "-nocompileeditor", "-ubtargs=-NoUBA -WaitMutex"], check=True)


if __name__ == "__main__":
    main()
