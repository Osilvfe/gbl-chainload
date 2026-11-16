#!/usr/bin/env bash
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

OS="${1:-}"
case "$OS" in
  windows|macos|linux|all) ;;
  *) echo "usage: $0 windows|macos|linux|all" >&2; exit 2 ;;
esac
[[ $# -eq 1 ]] || { echo "too many arguments" >&2; exit 2; }

require_tool() {
  command -v "$1" >/dev/null || { echo "missing build tool: $1" >&2; exit 1; }
}
require_target() {
  rustup target list --installed | grep -qx "$1" || {
    echo "missing Rust target: run rustup target add $1" >&2
    exit 1
  }
}
publish() {
  mkdir -p "dist/$1"
  if [[ "$2" != "dist/$1/$3" ]]; then cp "$2" "dist/$1/$3"; fi
  chmod +x "dist/$1/$3"
  python3 - "$1" "$3" <<'PY'
import hashlib
from pathlib import Path
import sys

out = Path("dist") / sys.argv[1]
name = sys.argv[2]
(out / "SHA256SUMS").write_text(hashlib.sha256((out / name).read_bytes()).hexdigest() + "  " + name + "\n")
PY
}

for tool in cargo rustup python3; do require_tool "$tool"; done
if [[ "$OS" == windows || "$OS" == all ]]; then
  require_target x86_64-pc-windows-gnu
  require_tool "${CARGO_TARGET_X86_64_PC_WINDOWS_GNU_LINKER:-x86_64-w64-mingw32-gcc}"
  require_tool x86_64-w64-mingw32-dlltool
fi
if [[ "$OS" == macos || "$OS" == all ]]; then
  require_target x86_64-apple-darwin
  require_target aarch64-apple-darwin
  LIPO="$(command -v lipo || command -v llvm-lipo || true)"
  [[ -n "$LIPO" ]] || { echo "missing lipo or llvm-lipo" >&2; exit 1; }
  if [[ "$(uname -s)" == Darwin ]]; then
    require_tool clang
  else
    require_tool zig
    ZIG="$(command -v zig)"
    WRAPPERS="$REPO_ROOT/Build/native-toolchain/bin"
    mkdir -p "$WRAPPERS"
    for arch in x86_64 aarch64; do
      wrapper="$WRAPPERS/zig-cc-macos-$arch"
      printf '#!/bin/sh\nexec "%s" cc -target %s-macos "$@"\n' "$ZIG" "$arch" > "$wrapper"
      chmod +x "$wrapper"
    done
    export CARGO_TARGET_X86_64_APPLE_DARWIN_LINKER="$WRAPPERS/zig-cc-macos-x86_64"
    export CARGO_TARGET_AARCH64_APPLE_DARWIN_LINKER="$WRAPPERS/zig-cc-macos-aarch64"
  fi
fi
if [[ "$OS" == linux || "$OS" == all ]]; then
  require_target x86_64-unknown-linux-musl
  require_tool "${CARGO_TARGET_X86_64_UNKNOWN_LINUX_MUSL_LINKER:-musl-gcc}"
fi

TARGET_DIR="${CARGO_TARGET_DIR:-target}"
if [[ "$OS" == windows || "$OS" == all ]]; then
  cargo build --release --locked --target x86_64-pc-windows-gnu -p gbl
  publish windows "$TARGET_DIR/x86_64-pc-windows-gnu/release/gbl.exe" gbl.exe
fi
if [[ "$OS" == macos || "$OS" == all ]]; then
  cargo build --release --locked --target x86_64-apple-darwin -p gbl
  cargo build --release --locked --target aarch64-apple-darwin -p gbl
  mkdir -p dist/macos
  "$LIPO" -create -output dist/macos/gbl \
    "$TARGET_DIR/x86_64-apple-darwin/release/gbl" \
    "$TARGET_DIR/aarch64-apple-darwin/release/gbl"
  publish macos dist/macos/gbl gbl
fi
if [[ "$OS" == linux || "$OS" == all ]]; then
  cargo build --release --locked --target x86_64-unknown-linux-musl -p gbl
  publish linux "$TARGET_DIR/x86_64-unknown-linux-musl/release/gbl" gbl
fi
echo "==> $OS build done"
