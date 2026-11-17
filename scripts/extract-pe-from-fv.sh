#!/usr/bin/env bash
# extract-pe-from-fv.sh — unwrap LinuxLoader.efi from each raw FV in local/firmware/
# into local/firmware/pe/<fixture>.efi.  Skip fixtures that already have a PE form.
set -euo pipefail

cd "$(dirname "$0")/.."
cargo build --release --quiet -p gbl
PATH="$PWD/target/release:$PATH"; export PATH

mkdir -p local/firmware/pe

# Per-fixture extraction.  Add new entries when more fixtures land.
declare -A FV_TO_PE=(
  ["local/firmware/infiniti-EU-16.0.5.703/abl.bin"]="local/firmware/pe/infiniti-EU-16.0.5.703.efi"
  ["local/firmware/infiniti-IN-16.0.7.201.img"]="local/firmware/pe/infiniti-IN-16.0.7.201.efi"
  ["local/firmware/fairlady-CN-16.0.7.200.img"]="local/firmware/pe/fairlady-CN-16.0.7.200.efi"
)

for src in "${!FV_TO_PE[@]}"; do
  dest="${FV_TO_PE[$src]}"
  if [[ -f "$src" ]]; then
    echo "==> $src → $dest"
    gbl unwrap "$src" "$dest" || {
      echo "WARN: failed to extract $src (FV may need LZMA decompression — out of scope)"; continue;
    }
    sha256sum "$dest"
  else
    echo "SKIP: $src (file missing)"
  fi
done

# Copy a pre-extracted myron PE if MYRON_PE points at one
# (e.g. MYRON_PE=/path/to/gbl_root_canoe/tests/extracted/LinuxLoader.efi).
if [[ -n "${MYRON_PE:-}" && -f "$MYRON_PE" ]]; then
  cp "$MYRON_PE" local/firmware/pe/myron.efi
  sha256sum local/firmware/pe/myron.efi
fi

echo "==> Done. PE fixtures available:"
ls -la local/firmware/pe/
