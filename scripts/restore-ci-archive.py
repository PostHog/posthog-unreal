#!/usr/bin/env python3
"""Restore a checksum-pinned ZIP from a protected CI URL without logging credentials."""
import argparse
import hashlib
import os
from pathlib import Path, PurePosixPath
import re
import stat
import subprocess
import tempfile
import urllib.request
import zipfile


def validate_archive(path):
    with zipfile.ZipFile(path) as archive:
        for entry in archive.infolist():
            name = PurePosixPath(entry.filename)
            if name.is_absolute() or ".." in name.parts or "\\" in entry.filename or ":" in entry.filename:
                raise ValueError("Unsafe archive member")
            if stat.S_ISLNK(entry.external_attr >> 16):
                # Platform extractors preserve links; only allow targets inside the archive root.
                target = PurePosixPath(archive.read(entry).decode())
                if target.is_absolute() or "\\" in str(target) or ":" in str(target):
                    raise ValueError("Unsafe archive symlink")
                depth = len(name.parts) - 1
                for part in target.parts:
                    depth += -1 if part == ".." else (0 if part == "." else 1)
                    if depth < 0:
                        raise ValueError("Archive symlink escapes its root")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    url, expected = os.environ.get("CI_ARCHIVE_URL", ""), os.environ.get("CI_ARCHIVE_SHA256", "")
    if not url.startswith("https://") or not re.fullmatch(r"[0-9a-fA-F]{64}", expected):
        parser.error("Configure CI_ARCHIVE_URL (HTTPS) and CI_ARCHIVE_SHA256")
    destination = args.destination.resolve()
    if destination.exists():
        parser.error("Destination must not exist")
    with tempfile.TemporaryDirectory() as temporary:
        path = Path(temporary) / "archive.zip"
        digest = hashlib.sha256()
        try:
            with urllib.request.urlopen(url, timeout=120) as response, path.open("wb") as output:
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
        except Exception:
            # Signed URLs may contain credentials. Do not include exception URLs in CI logs.
            raise SystemExit("CI archive download failed; verify access to the configured artifact") from None
        if digest.hexdigest() != expected.lower():
            raise SystemExit("CI archive SHA-256 mismatch")
        validate_archive(path)
        destination.mkdir(parents=True)
        if os.name == "nt":
            command = ["tar", "-xf", str(path), "-C", str(destination)]
        else:
            command = ["unzip", "-q", str(path), "-d", str(destination)]
        subprocess.run(command, check=True)


if __name__ == "__main__":
    main()
