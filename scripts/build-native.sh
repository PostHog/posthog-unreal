#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-$(rustc -vV | awk '/^host:/ {print $2}')}"
PYTHON="${PYTHON:-python3}"
"$PYTHON" -c 'import tomllib' || { echo 'Python 3.11+ is required (set PYTHON).' >&2; exit 1; }
case "$TARGET" in
  aarch64-apple-darwin|x86_64-apple-darwin)
    LIB=libposthog_unreal_ffi.a
    export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-14.0}"
    ;;
  x86_64-unknown-linux-gnu) LIB=libposthog_unreal_ffi.a ;;
  x86_64-pc-windows-msvc) LIB=posthog_unreal_ffi.lib ;;
  *) echo "Unsupported native target: $TARGET" >&2; exit 1 ;;
esac
export CARGO_NET_GIT_FETCH_WITH_CLI=true
# Until the embedding APIs land upstream, build against the implementation worktree.
: "${POSTHOG_RS_PATH:?Set POSTHOG_RS_PATH to the posthog-rs v1 embedding checkout}"
PATCH="$("$PYTHON" -c 'import json,os; print("patch.\"ssh://git@github.com/PostHog/posthog-rs.git\".posthog-rs.path=" + json.dumps(os.environ["POSTHOG_RS_PATH"]))')"
cargo build --config "$PATCH" --manifest-path "$ROOT/rust/Cargo.toml" --locked --release --target "$TARGET"
DEST="$ROOT/Source/ThirdParty/PostHogRust/lib/$TARGET"
mkdir -p "$DEST"
cp "$ROOT/rust/target/$TARGET/release/$LIB" "$DEST/$LIB"
"$PYTHON" - "$ROOT" "$DEST/$LIB" "$TARGET" <<'PY'
import hashlib, json, os, pathlib, subprocess, sys, tomllib
root, lib, target = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
lock = (root / 'rust/Cargo.lock').read_bytes()
core = next(p for p in tomllib.loads(lock.decode())['package'] if p['name'] == 'posthog-rs')
core_root = pathlib.Path(os.environ['POSTHOG_RS_PATH']).resolve()
source_hash = hashlib.sha256()
for source in sorted([core_root / 'Cargo.toml', core_root / 'Cargo.lock', *core_root.joinpath('src').rglob('*.rs')]):
    source_hash.update(str(source.relative_to(core_root)).encode() + b'\0')
    source_hash.update(source.read_bytes() + b'\0')
manifest = {
    'abi_version': 2, 'bridge_version': '0.0.1', 'core_version': core['version'],
    'core_source': core.get('source', 'local embedding checkout'), 'target': target,
    'core_base': subprocess.check_output(['git', '-C', str(core_root), 'rev-parse', 'HEAD'], text=True).strip(),
    'core_source_sha256': source_hash.hexdigest(),
    'rustc': subprocess.check_output(['rustc', '--version'], text=True).strip(),
    'sha256': hashlib.sha256(lib.read_bytes()).hexdigest(),
    'lockfile_sha256': hashlib.sha256(lock).hexdigest(),
    'core_default_features': False, 'core_features': ['tls', 'embedding'],
    'ingestion': 'v1', 'analytics_endpoint': '/i/v1/analytics/events',
}
lib.with_name('manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
PY
