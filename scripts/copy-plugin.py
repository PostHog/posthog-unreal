#!/usr/bin/env python3
"""Install a managed source-only plugin copy without nested test projects or build rules."""
import argparse
from pathlib import Path
import shutil

ROOT = Path(__file__).resolve().parents[1]


def copy_plugin(destination):
    destination = Path(destination).absolute()
    marker = destination / ".posthog-source-copy"
    if destination.is_symlink():
        if destination.resolve() != ROOT:
            raise RuntimeError(f"Refusing to replace an unrelated plugin link: {destination}")
        destination.unlink()
    elif destination.exists():
        if not marker.is_file():
            raise RuntimeError(f"Refusing to replace an unmanaged plugin: {destination}")
        # Compiled products remain available for incremental Unreal builds.
        if (destination / "Source").exists():
            shutil.rmtree(destination / "Source")
    destination.mkdir(parents=True, exist_ok=True)
    shutil.copytree(ROOT / "Source", destination / "Source")
    shutil.copy2(ROOT / "PostHog.uplugin", destination / "PostHog.uplugin")
    marker.touch()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    copy_plugin(parser.parse_args().destination)
