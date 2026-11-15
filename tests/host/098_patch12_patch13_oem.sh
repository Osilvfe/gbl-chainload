#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

cargo build --release --quiet -p gbl -p patch-engine --features patch-engine/host
OUT=tests/host/.last/098
mkdir -p "$OUT"
rustc --edition=2021 -C panic=abort tests/host/helpers/patch_oem_scope.rs \
  --extern patch_engine=target/release/libpatch_engine.rlib \
  -L dependency=target/release/deps -o "$OUT/check"

shopt -s nullglob
images=(tests/images/*-abl.img)
[ "${#images[@]}" -gt 0 ] || { echo "SKIP: 098 — no ABL fixtures present"; exit 0; }
pes=()

for image in "${images[@]}"; do
  name=$(basename "$image" .img)
  pe="$OUT/$name.efi"
  pes+=("$pe")
  target/release/gbl unwrap "$image" "$pe" >"$OUT/$name.unwrap.log" 2>&1
  target/release/gbl patch --in "$pe" --oem none --out "$OUT/$name.none.efi" \
    >"$OUT/$name.none.log" 2>&1
  target/release/gbl patch --in "$pe" --out "$OUT/$name.plain.efi" \
    >"$OUT/$name.plain.log" 2>&1
  cmp "$OUT/$name.none.efi" "$OUT/$name.plain.efi"
  for logfile in "$OUT/$name.none.log" "$OUT/$name.plain.log"; do
    if grep -qE 'patch(7|12|13)-' "$logfile"; then
      echo "FAIL: $name — OEM patch attempted without --oem oplus"
      cat "$logfile"
      exit 1
    fi
  done

  if [[ "$name" == op* ]]; then
    target/release/gbl patch --in "$pe" --oem oplus --out "$OUT/$name.selected.efi" \
      >"$OUT/$name.selected.log" 2>&1
    for patch in patch12-boot-failure-fastboot-fallback patch13-slot-unbootable-inert; do
      grep -qF "$patch [oem-oplus, optional] -> OK" "$OUT/$name.selected.log" \
        || { echo "FAIL: $name — $patch missing"; cat "$OUT/$name.selected.log"; exit 1; }
    done
    target/release/gbl patch --in "$OUT/$name.selected.efi" --oem oplus \
      >"$OUT/$name.reapply.log" 2>&1 || true
    for patch in patch12-boot-failure-fastboot-fallback patch13-slot-unbootable-inert; do
      grep -qF "$patch [oem-oplus, optional] -> OK" "$OUT/$name.reapply.log" \
        || { echo "FAIL: $name — $patch not idempotent"; cat "$OUT/$name.reapply.log"; exit 1; }
    done
  else
    cp "$OUT/$name.none.efi" "$OUT/$name.selected.efi"
  fi
done

"$OUT/check" "${pes[@]}" >"$OUT/matrix.log" 2>"$OUT/engine.log"
cat "$OUT/matrix.log"
