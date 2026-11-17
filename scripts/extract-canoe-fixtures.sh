#!/usr/bin/env bash
# Extract fixture binaries used by tests/042 anchor-uniqueness CI.
# Inputs: stock OTA partition images dropped into local/firmware/canoe-stock-A.07_2024_02_05/
# Outputs: local/firmware/fixtures/canoe-A.07/abl_a.bin (a private verbatim copy)
set -euo pipefail

SRC_DIR="${1:-local/firmware/canoe-stock-A.07_2024_02_05}"
DEST_DIR="${2:-local/firmware/fixtures/canoe-A.07}"

# Resolve relative to repo root if not absolute.
if [[ "$SRC_DIR" != /* ]]; then
  cd "$(dirname "$0")/.."
fi

if [[ ! -f "$SRC_DIR/abl_a.bin" && ! -f "$SRC_DIR/abl_a.img" ]]; then
  echo "ERROR: $SRC_DIR/abl_a.{bin,img} not found." >&2
  echo "Drop a stock A.07 OTA's abl_a partition image at that path." >&2
  exit 1
fi

mkdir -p "$DEST_DIR"
if [[ -f "$SRC_DIR/abl_a.bin" ]]; then
  cp -f "$SRC_DIR/abl_a.bin" "$DEST_DIR/abl_a.bin"
else
  cp -f "$SRC_DIR/abl_a.img" "$DEST_DIR/abl_a.bin"
fi
sha256sum "$DEST_DIR/abl_a.bin"
echo "OK: fixture at $DEST_DIR/abl_a.bin"
