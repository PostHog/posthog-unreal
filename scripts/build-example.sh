#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
: "${UE_ROOT:?Set UE_ROOT to the Unreal Engine installation directory}"
PROJECT="$ROOT/Examples/PostHogExample"
mkdir -p "$PROJECT/Plugins"
if [ ! -e "$PROJECT/Plugins/PostHog" ]; then
    ln -s "$ROOT" "$PROJECT/Plugins/PostHog"
fi
"$UE_ROOT/Engine/Build/BatchFiles/Mac/Build.sh" PostHogExampleEditor Mac Development "$PROJECT/PostHogExample.uproject" -architecture=arm64 -WaitMutex -NoUBA -NoHotReload
