#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

AUTO=0
DEBUG=0
VERBOSE=0
DO_RECOVERY=1
DO_HOST=1
while [[ $# -gt 0 ]]; do
  case "$1" in
    --auto) AUTO=1; shift ;;
    --debug) DEBUG=1; shift ;;
    --verbose) VERBOSE=1; shift ;;
    --no-recovery) DO_RECOVERY=0; shift ;;
    --no-host) DO_HOST=0; shift ;;
    -h|--help)
      cat <<EOF
Usage: $0 [--auto] [--debug] [--verbose] [--no-recovery] [--no-host]

Build with locally installed toolchains:
  1. Firmware staticlibs (Cargo, aarch64-unknown-none)
  2. EDK2 firmware (AArch64 GNU toolchain)
  3. Android recovery tool (Android NDK); skip with --no-recovery
  4. Native host tool (Cargo); skip with --no-host

Firmware defaults to release logging. --auto, --debug and --verbose
control entry policy and logging. Install mode is selected by the
runtime GBLP1 manifest, not by a build-time --mode flag.
Build/ caches, including UniXNU components, are preserved.
EOF
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

SUFFIX=""
ARGS=(--release)
if [[ $AUTO -eq 1 ]]; then SUFFIX+="-auto"; ARGS+=(--auto); fi
if [[ $DEBUG -eq 1 ]]; then SUFFIX+="-debug"; ARGS+=(--debug); fi
if [[ $VERBOSE -eq 1 ]]; then SUFFIX+="-verbose"; ARGS+=(--verbose); fi
ARTIFACT="dist/gbl-chainload${SUFFIX}.efi"

if [[ $DO_RECOVERY -eq 1 ]]; then
  bash scripts/build-recovery-tools.sh --check-only
fi

echo "==> [1/4 + 2/4] Building $ARTIFACT"
bash scripts/build-native.sh "${ARGS[@]}" --output "$ARTIFACT"

if [[ $DO_RECOVERY -eq 1 ]]; then
  echo "==> [3/4] Building Android recovery tool"
  bash scripts/build-recovery-tools.sh
else
  echo "==> [3/4] SKIP recovery cross-build (--no-recovery)"
fi

if [[ $DO_HOST -eq 1 ]]; then
  echo "==> [4/4] Building native host tool"
  cargo build --release --locked -p gbl
  mkdir -p dist/host
  cp "${CARGO_TARGET_DIR:-target}/release/gbl" dist/host/gbl
else
  echo "==> [4/4] SKIP host build (--no-host)"
fi

echo "==> done."
echo "    firmware: $ARTIFACT"
if [[ $DO_RECOVERY -eq 1 ]]; then echo "    recovery: dist/recovery/gbl"; fi
if [[ $DO_HOST -eq 1 ]]; then echo "    host:     dist/host/gbl"; fi
