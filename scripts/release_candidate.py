#!/usr/bin/env python3
"""Apply exactly the release candidate prepared by the read-only job."""
import argparse
import hashlib
from pathlib import Path
import subprocess

import release


def apply_candidate(patch, source, digest, tree, version, root=release.ROOT):
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=root, text=True).strip()

    if git("rev-parse", "HEAD") != source:
        raise ValueError("Checkout does not match the release candidate source")
    if hashlib.sha256(patch.read_bytes()).hexdigest() != digest:
        raise ValueError("Release candidate patch checksum mismatch")
    subprocess.run(["git", "apply", "--index", str(patch.resolve())], cwd=root, check=True)
    if git("write-tree") != tree:
        raise ValueError("Applied tree does not match the release candidate")
    subprocess.run(["git", "diff", "--exit-code"], cwd=root, check=True)
    if release.get_version(root) != version:
        raise ValueError("Applied version does not match the release candidate")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("patch", type=Path)
    parser.add_argument("--source", required=True)
    parser.add_argument("--sha256", required=True)
    parser.add_argument("--tree", required=True)
    parser.add_argument("--version", required=True)
    args = parser.parse_args()
    apply_candidate(args.patch, args.source, args.sha256, args.tree, args.version)


if __name__ == "__main__":
    main()
