#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-$(rustc -vV | awk '/^host:/ {print $2}')}"
if [ "$#" -gt 0 ]; then shift; fi
exec "${PYTHON:-python3}" "$ROOT/scripts/build-native.py" "$TARGET" "$@"
