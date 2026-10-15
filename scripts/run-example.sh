#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${UE_ROOT:?Set UE_ROOT to the Unreal Engine installation directory}"
exec "$UE_ROOT/Engine/Binaries/Mac/UnrealEditor.app/Contents/MacOS/UnrealEditor" \
    "$ROOT/Examples/PostHogExample/PostHogExample.uproject" \
    "$@"
