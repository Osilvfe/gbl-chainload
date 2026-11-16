#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

CHECK_ONLY=0
case "${1:-}" in
  "") ;;
  --check-only) CHECK_ONLY=1 ;;
  -h|--help)
    echo "Usage: $0 [--check-only]"
    echo "Set ANDROID_NDK_HOME (or ANDROID_NDK_ROOT / ANDROID_NDK), NDK_BIN,"
    echo "or CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER to a local Android NDK."
    exit 0 ;;
  *) echo "unknown flag: $1" >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { echo "too many arguments" >&2; exit 2; }

for tool in cargo rustup python3; do
  command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done
rustup target list --installed | grep -qx aarch64-linux-android || {
  echo "missing Rust target: run rustup target add aarch64-linux-android" >&2
  exit 1
}

NDK_ROOT="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${ANDROID_NDK:-}}}"
if [[ -z "${NDK_BIN:-}" && -n "$NDK_ROOT" ]]; then
  case "$(uname -s)" in
    Linux) NDK_HOST=linux-x86_64 ;;
    Darwin) NDK_HOST=darwin-x86_64 ;;
    *) echo "unsupported Android NDK host: $(uname -s)" >&2; exit 1 ;;
  esac
  NDK_BIN="$NDK_ROOT/toolchains/llvm/prebuilt/$NDK_HOST/bin"
fi
if [[ -n "${NDK_BIN:-}" ]]; then
  export PATH="$NDK_BIN:$PATH"
fi
LINKER="${CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER:-aarch64-linux-android31-clang}"
command -v "$LINKER" >/dev/null || {
  echo "Android NDK linker not found: $LINKER" >&2
  echo "Set ANDROID_NDK_HOME to an installed NDK, or skip recovery with --no-recovery." >&2
  exit 1
}
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$LINKER"

if [[ $CHECK_ONLY -eq 1 ]]; then exit 0; fi

cargo build --release --locked --target aarch64-linux-android -p gbl
mkdir -p dist/recovery
cp "${CARGO_TARGET_DIR:-target}/aarch64-linux-android/release/gbl" dist/recovery/gbl
chmod +x dist/recovery/gbl
python3 - <<'PY'
import hashlib
from pathlib import Path

out = Path("dist/recovery")
(out / "SHA256SUMS").write_text(hashlib.sha256((out / "gbl").read_bytes()).hexdigest() + "  gbl\n")
PY
echo "==> wrote dist/recovery/gbl"
