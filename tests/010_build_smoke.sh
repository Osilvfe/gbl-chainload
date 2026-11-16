#!/usr/bin/env bash
# 010_build_smoke.sh — verify scripts/build.sh produces the expected artifact.
#
# Engine rework (Task 11): build.sh produces a single dist/gbl-chainload.efi
# (and dist/gbl-chainload-<suffix>.efi when --auto/--debug/--verbose). The
# per-mode loop is gone — activation lives in the runtime GBLP1 manifest.
#
# Requires local Rust, AArch64 GCC and EDK2. If prerequisites
# are unavailable on the runner, skip cleanly — host-side lint/scan/patch tests
# still run, the EFI compile path simply isn't validated here.
set -euo pipefail
cd "$(dirname "$0")/.."
fail=0

for tool in cargo rustup make python3 aarch64-linux-gnu-gcc; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "SKIP: 010_build_smoke — missing $tool"
    exit 0
  fi
done
if [[ ! -d edk2/BaseTools ]] || ! rustup target list --installed | grep -qx aarch64-unknown-none; then
  echo "SKIP: 010_build_smoke — initialize edk2 and install Rust target aarch64-unknown-none"
  exit 0
fi

echo "== building dist/gbl-chainload-auto-debug-verbose.efi =="
./scripts/build.sh --auto --debug --verbose --no-recovery --no-host
test -f dist/gbl-chainload-auto-debug-verbose.efi \
  || { echo "FAIL: dist/gbl-chainload-auto-debug-verbose.efi missing"; exit 1; }

echo "== building dist/gbl-chainload.efi =="
./scripts/build.sh --no-recovery --no-host
test -f dist/gbl-chainload.efi || { echo "FAIL: dist/gbl-chainload.efi missing"; exit 1; }


# ── VERBOSE compile-strip verification ────────────────────────────────────
# VERBOSE(fmt, ...) compile-strips to a no-op under GBL_VERBOSE=0, so the
# format strings of VERBOSE call sites must be ABSENT from .rodata of
# non-verbose builds. They appear in --verbose builds.
#
# Use multiple probe fragments so one missing isn't a false PASS.
echo "--- VERBOSE strip verification ---"
PROBES=(
  'section @ 0x'      # AblUnwrap per-section scan
  'qsee-buf'          # QseecomHook payload hex
  'first16='          # VerifiedBootHook payload hex
)

# Non-verbose artifact is dist/gbl-chainload.efi (no --verbose flag passed).
# VERBOSE() format-string fragments must be absent from its .rodata.
for v in dist/gbl-chainload.efi; do
  [ -f "$v" ] || continue
  for p in "${PROBES[@]}"; do
    n=$(strings "$v" 2>/dev/null | grep -c "$p" || true)
    if [ "$n" -gt 0 ]; then
      echo "FAIL: VERBOSE probe '$p' found $n time(s) in non-verbose build $v" >&2
      fail=1
    fi
  done
done

if [ -f dist/gbl-chainload-auto-debug-verbose.efi ]; then
  total=0
  for p in "${PROBES[@]}"; do
    n=$(strings dist/gbl-chainload-auto-debug-verbose.efi 2>/dev/null | grep -c "$p" || true)
    total=$((total + n))
  done
  if [ "$total" -eq 0 ]; then
    echo "WARN: no VERBOSE probe markers found in gbl-chainload-auto-debug-verbose.efi —"  >&2
    echo "      compiler may have stripped string literals; manual nm/objdump needed" >&2
  else
    echo "OK: $total VERBOSE probe marker(s) present in verbose build"
  fi
fi
echo "--- end VERBOSE strip verification ---"

[ "$fail" -eq 0 ] && echo "ok 010_build_smoke" || { echo "FAIL: 010_build_smoke"; exit 1; }
