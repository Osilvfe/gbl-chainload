#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

for tool in cargo rustup python3 file; do
  command -v "$tool" >/dev/null || { echo "SKIP: 084 — missing $tool"; exit 0; }
done

targets="$(rustup target list --installed)"
if command -v x86_64-w64-mingw32-gcc >/dev/null &&
   command -v x86_64-w64-mingw32-dlltool >/dev/null &&
   grep -qx x86_64-pc-windows-gnu <<< "$targets"; then
  bash scripts/build-cross-tools.sh windows
  format="$(file -b dist/windows/gbl.exe)"
  case "$format" in
    *PE32+*x86-64*) echo "PASS: 084 Windows PE32+ x86-64" ;;
    *) echo "FAIL: unexpected Windows binary: $format"; exit 1 ;;
  esac
else
  echo "SKIP: 084 Windows — install mingw-w64 and Rust target x86_64-pc-windows-gnu"
fi

if { [[ "$(uname -s)" == Darwin ]] || command -v zig >/dev/null; } &&
   { command -v lipo >/dev/null || command -v llvm-lipo >/dev/null; } &&
   grep -qx x86_64-apple-darwin <<< "$targets" &&
   grep -qx aarch64-apple-darwin <<< "$targets"; then
  bash scripts/build-cross-tools.sh macos
  format="$(file -b dist/macos/gbl)"
  case "$format" in
    *"Mach-O universal"*x86_64*arm64*|*"Mach-O universal"*arm64*x86_64*)
      echo "PASS: 084 macOS universal x86_64 + arm64" ;;
    *) echo "FAIL: unexpected macOS binary: $format"; exit 1 ;;
  esac
else
  echo "SKIP: 084 macOS — install Apple toolchains (or Zig), lipo and both Rust targets"
fi

if command -v musl-gcc >/dev/null && grep -qx x86_64-unknown-linux-musl <<< "$targets"; then
  bash scripts/build-cross-tools.sh linux
  format="$(file -b dist/linux/gbl)"
  case "$format" in
    *ELF*64-bit*x86-64*static*) echo "PASS: 084 static Linux x86-64" ;;
    *) echo "FAIL: unexpected Linux binary: $format"; exit 1 ;;
  esac
else
  echo "SKIP: 084 Linux — install musl-tools and Rust target x86_64-unknown-linux-musl"
fi
