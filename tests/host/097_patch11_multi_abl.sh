#!/usr/bin/env bash
# tests/host/097_patch11_multi_abl.sh — patch11 (locked-handoff) cross-build gate.
#
# patch11 is the 方案1 "lifecycle split" spoof: ABL stays internally
# unlocked (patch10's permissive path keeps working) while every
# outbound handoff surface reports locked/green:
#   11a  `androidboot.vbmeta.device_state` producer — the unique string
#        anchor's CSEL (byte-identical 0x9A890142 across all inspected
#        builds) is rewritten to `MOV x2, x10` so the value is always
#        "locked".
#   11b  the verified-boot color table ({0:green,1:orange,2:yellow,
#        3:red} @16-byte stride in .data) has its orange and yellow
#        slots repointed at the green string, so every
#        `*verifiedbootstate` consumer (bootconfig entry, cmdline and
#        oplusboot mirrors, debug prints) answers "green".
#
# Unlike patch7, patch11 anchors on AOSP-shared surfaces — it applies to
# non-oplus (Xiaomi) ABLs too, and that is asserted here.
#
# Idempotency note: like patch7, patch11 detects its own already-applied
# state and returns OK on re-apply. patch10/patch6 lack that check and
# mandatory-miss on a pre-patched PE (abl-patcher exits non-zero), so
# pass 2 runs under `|| true` and asserts only the patch11 line.
#
# Uses the tracked tests/images/ ABL fixtures, so it runs in CI (not SKIP).
set -euo pipefail
cd "$(dirname "$0")/../.."

cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH
FV=(gbl unwrap)
PATCHER=(gbl patch)

OUT=tests/host/.last/097
mkdir -p "$OUT"

ALL_ABLS=(
  tests/images/op15-infiniti-703-abl.img
  tests/images/op15-infiniti-201-abl.img
  tests/images/op15t-fairlady-201-abl.img
  tests/images/xi17-pudding-44-abl.img
)

ran=0

for img in "${ALL_ABLS[@]}"; do
  [ -f "$img" ] || { echo "SKIP: $img missing"; continue; }
  ran=1
  name=$(basename "$img" .img)
  pe="$OUT/$name.pe.efi"
  p1="$OUT/$name.p1.efi"

  "${FV[@]}" "$img" "$pe" >"$OUT/$name.unwrap.log" 2>&1 \
      || { echo "FAIL: fv-unwrap $img"; cat "$OUT/$name.unwrap.log"; exit 1; }

  # 1. First application: patch11 must -> OK on every ABL (oplus and
  #    non-oplus alike — the anchors are AOSP-shared).
  "${PATCHER[@]}" --in "$pe" --out "$p1" --oem oplus \
      >"$OUT/$name.p1.log" 2>&1 \
      || { echo "FAIL: $name abl-patcher (pass 1)"; cat "$OUT/$name.p1.log"; exit 1; }
  if ! grep -qE 'patch11-locked-handoff-bootconfig .* -> OK' "$OUT/$name.p1.log"; then
    echo "FAIL: $name patch11 did not apply (pass 1)"; cat "$OUT/$name.p1.log"; exit 1
  fi

  # 2. Idempotency: re-run against the already-patched PE. patch10 +
  #    patch6 mandatory-miss (expected); the invariant under test is
  #    patch11 reporting OK again with no double-mutation.
  "${PATCHER[@]}" --in "$p1" --oem oplus \
      >"$OUT/$name.p2.log" 2>&1 \
      || true
  if ! grep -qE 'patch11-locked-handoff-bootconfig .* -> OK' "$OUT/$name.p2.log"; then
    echo "FAIL: $name patch11 not idempotent on re-apply"
    cat "$OUT/$name.p2.log"; exit 1
  fi

  # 3. Byte-level effect, build-independent. The frozen CSEL word
  #    (0x9A890142 LE = 42 01 89 9a) can occur incidentally elsewhere in
  #    the image, so assert on occurrence counts: patched has exactly
  #    one CSEL word fewer than pristine, and gains at least one
  #    occurrence of the MOV x2, x10 rewrite (0xAA0A03E2 LE = e2 03 0a
  #    aa). Skipped when python3 is unavailable.
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$pe" "$p1" <<'PYEOF'
import sys
def count(path, pat):
    b = open(path, 'rb').read()
    n, i = 0, 0
    while True:
        i = b.find(pat, i)
        if i < 0: return n
        n += 1; i += 1
pristine, patched = sys.argv[1], sys.argv[2]
csel = bytes.fromhex('4201899a')
mov = bytes.fromhex('e2030aaa')
cp, cc = count(patched, csel), count(pristine, csel)
if cc == 0:
    sys.exit(f'FAIL: pristine PE lacks the frozen CSEL word (count=0)')
if cp != cc - 1:
    sys.exit(f'FAIL: CSEL word count pristine={cc} patched={cp} (expected -1)')
if count(patched, mov) <= count(pristine, mov):
    sys.exit('FAIL: patched PE did not gain a MOV x2, x10 word')
PYEOF
  fi
  echo "  ok: $name — patch11 applied + idempotent; CSEL rewritten"
done

if [ "$ran" -eq 0 ]; then
  echo "SKIP: no ABL fixtures present"; exit 0
fi

echo "PASS: patch11 cross-build gate"
