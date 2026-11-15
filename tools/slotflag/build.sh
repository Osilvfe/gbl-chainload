#!/usr/bin/env bash
# Build the gbl-slotflag KernelSU module zip.
#
# The module ships the same `gbl` multicall binary that the host tool is:
# `gbl slot show|set|backup|restore`. One implementation, so the device and
# the test suite can never drift apart.
#
#   ./tools/slotflag/build.sh
#
# Requires the aarch64 cross toolchain:
#   rustup target add aarch64-unknown-linux-gnu
#   aarch64-linux-gnu-gcc
set -euo pipefail
cd "$(dirname "$0")"

ROOT=$(cd ../.. && pwd)
OUTDIR=${OUTDIR:-$ROOT/dist}
NAME=gbl-slotflag
VER=$(sed -n 's/^version=//p' module/module.prop)
TARGET=${TARGET:-aarch64-unknown-linux-gnu}
LINKER=${LINKER:-aarch64-linux-gnu-gcc}

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo ">>> cargo build -p gbl --release --target $TARGET"
RUSTFLAGS="-C target-feature=+crt-static -C linker=$LINKER" \
  cargo build --manifest-path "$ROOT/Cargo.toml" \
              -p gbl --release --target "$TARGET"
BIN="$ROOT/target/$TARGET/release/gbl"
[ -x "$BIN" ] || { echo "error: $BIN missing" >&2; exit 1; }

mkdir -p "$WORK/mod"
cp -r module/. "$WORK/mod/"
mkdir -p "$WORK/mod/bin"
cp "$BIN" "$WORK/mod/bin/gbl"
"${TARGET}-strip" "$WORK/mod/bin/gbl" 2>/dev/null \
  || aarch64-linux-gnu-strip "$WORK/mod/bin/gbl" 2>/dev/null || true

file "$WORK/mod/bin/gbl"
# The aarch64 binary cannot run on an x86_64 host; verify it is an ARM ELF
# instead of pretending a smoke test happened.
case "$(file -b "$WORK/mod/bin/gbl")" in
  *aarch64*ELF*|*ELF*aarch64*) ;;
  *) echo "error: unexpected binary format" >&2; exit 1 ;;
esac

chmod 0755 "$WORK/mod/bin/gbl" "$WORK/mod/bin/slotctl" \
            "$WORK/mod/customize.sh" "$WORK/mod/action.sh" \
            "$WORK/mod/uninstall.sh"
# webroot/ permissions are left to the manager on purpose: it sets both the
# mode and the SELinux context, and overriding them breaks the WebUI. The
# installer no longer touches them either.

mkdir -p "$OUTDIR"
ZIP="$OUTDIR/$NAME-$VER.zip"
rm -f "$ZIP"
python3 - "$WORK/mod" "$ZIP" <<'PY'
import os, sys, zipfile
root, out = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, 9) as z:
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, root))
PY
echo ">>> $ZIP"
unzip -l "$ZIP" | tail -10
