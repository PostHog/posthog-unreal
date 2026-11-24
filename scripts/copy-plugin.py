#!/usr/bin/env python3
"""Sync plugin descriptor and sources into an Unreal project's Plugins/PostHog directory."""
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: copy-plugin.py <destination-plugin-dir>")
    dest = Path(sys.argv[1]).resolve()
    dest.mkdir(parents=True, exist_ok=True)
    shutil.copy2(ROOT / "PostHog.uplugin", dest / "PostHog.uplugin")
    dest_source = dest / "Source"
    if dest_source.exists():
        shutil.rmtree(dest_source)
    shutil.copytree(ROOT / "Source", dest_source)


if __name__ == "__main__":
    main()
