#!/usr/bin/env bash
# Build the GBL firmware directly on the host (no Docker).
#
# The pinned EDK2 tree is old enough that current GCC defaults (C23) and its
# LTO output need two local accommodations:
#   * a GCC wrapper pins -std=gnu17 and -Wno-error;
#   * a local GccBase.lds drops .eh_frame/.eh_frame_hdr from Rust staticlibs.
#     GenFw cannot convert those sections when they end up outside .text.
#
# Neither adjustment changes the source submodule.  Both are generated under
# Build/, which is gitignored.
#
# Usage: scripts/build-native.sh [--release] [--output PATH]
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"

DEBUG=1
OUTPUT=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --release) DEBUG=0; shift ;;
    --output)  OUTPUT="${2:?--output requires a path}"; shift 2 ;;
    -h|--help)
      sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

for tool in cargo make python3; do
  command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done
command -v aarch64-linux-gnu-gcc >/dev/null || {
  echo "missing aarch64-linux-gnu-gcc (install gcc-aarch64-linux-gnu)" >&2
  exit 1
}
[[ -d edk2/BaseTools ]] || {
  echo "edk2 submodule is not initialized (expected edk2/BaseTools)" >&2
  exit 1
}

# The EDK2 submodule is shared with other checkouts and may be reset between
# runs.  Keep every EDK2-side change in a tracked patch and apply it
# idempotently instead of relying on the submodule worktree staying dirty.
BOOTFIX_PATCH="$REPO_ROOT/scripts/edk2/gbl-edk2-fixes.patch"
if git -C edk2 apply --reverse --check "$BOOTFIX_PATCH" 2>/dev/null; then
  echo ">>> EDK2 fixes already applied"
else
  echo ">>> applying EDK2 fixes"
  git -C edk2 apply "$BOOTFIX_PATCH"
fi

GBL_CHAINLOAD_VERSION="$(head -n1 VERSION | tr -d '[:space:]')"
[[ -n "$GBL_CHAINLOAD_VERSION" ]] || { echo "VERSION is empty" >&2; exit 1; }

SUFFIX=""
[[ $DEBUG -eq 1 ]] && SUFFIX="-debug"
BUILD_NAME="gbl-chainload${SUFFIX}"
OUTPUT="${OUTPUT:-dist/${BUILD_NAME}-gcc5.efi}"

# Derive a writable wrapper from the system cross compiler.
TC_BIN="$REPO_ROOT/Build/native-toolchain/bin"
mkdir -p "$TC_BIN"
GCC_PATH="$(command -v aarch64-linux-gnu-gcc)"
cat > "$TC_BIN/aarch64-linux-gnu-gcc" <<EOF
#!/bin/sh
exec "$GCC_PATH" "\$@" -std=gnu17 -Wno-error
EOF
chmod +x "$TC_BIN/aarch64-linux-gnu-gcc"
for name in ld ar as nm objcopy objdump strip ranlib readelf gcc-ar gcc-nm gcc-ranlib; do
  src="$(command -v "aarch64-linux-gnu-$name" || true)"
  [[ -n "$src" ]] && ln -sfn "$src" "$TC_BIN/aarch64-linux-gnu-$name"
done

if [[ ! -x edk2/BaseTools/Source/C/bin/GenFw ]]; then
  echo ">>> building EDK2 BaseTools"
  make -C edk2/BaseTools -j"$(nproc)" \
    EXTRA_OPTFLAGS="-Wno-error" COPT="-O -std=gnu17"
fi

# Mirror BaseTools so only the linker script differs from the pinned tree.
EDK_TOOLS="$REPO_ROOT/Build/native-edk-tools"
rm -rf "$EDK_TOOLS"
mkdir -p "$EDK_TOOLS"
for entry in edk2/BaseTools/*; do
  name="$(basename "$entry")"
  [[ "$name" == Scripts ]] && continue
  ln -s "$entry" "$EDK_TOOLS/$name"
done
cp -a edk2/BaseTools/Scripts "$EDK_TOOLS/Scripts"
cp scripts/edk2/GccBase.lds "$EDK_TOOLS/Scripts/GccBase.lds"

echo ">>> cargo staticlibs (aarch64-unknown-none)"
for crate in gblp1 mode2-profile-core patch-engine avb-parse; do
  cargo build --release --target aarch64-unknown-none \
    -p "$crate" --no-default-features
done

export WORKSPACE="$REPO_ROOT"
export PACKAGES_PATH="$REPO_ROOT:$REPO_ROOT/edk2"
export EDK_TOOLS_PATH="$EDK_TOOLS"
export CONF_PATH="$REPO_ROOT/Build/Conf"
export GCC5_AARCH64_PREFIX="$TC_BIN/aarch64-linux-gnu-"
export PATH="$REPO_ROOT/edk2/BaseTools/BinWrappers/PosixLike:$REPO_ROOT/edk2/BaseTools/Source/C/bin:$PATH"
mkdir -p "$CONF_PATH"

# Force a relink: make does not track the linker script as a dependency.
MOD=Build/GblChainloadPkg/RELEASE_GCC5/AARCH64/GblChainloadPkg/Application/GblChainload/GblChainload
rm -f "$MOD/DEBUG/GblChainload.dll" "$MOD/OUTPUT/GblChainload.efi" \
      "$MOD/DEBUG/GblChainload.efi"

echo ">>> EDK2 link: RELEASE GBL_DEBUG=$DEBUG"
build \
  -p GblChainloadPkg/GblChainloadPkg.dsc \
  -a AARCH64 -t GCC5 -b RELEASE \
  -D GBL_AUTO=0 -D GBL_DEBUG="$DEBUG" -D GBL_VERBOSE=0 \
  -D GBL_BUILD_NAME="$BUILD_NAME" \
  -D GBL_CHAINLOAD_VERSION="$GBL_CHAINLOAD_VERSION"

EFI="$MOD/OUTPUT/GblChainload.efi"
[[ -f "$EFI" ]] || { echo "build did not produce $EFI" >&2; exit 1; }
mkdir -p "$(dirname "$OUTPUT")"
cp "$EFI" "$OUTPUT"
echo ">>> wrote $OUTPUT ($(stat -c%s "$OUTPUT") bytes)"
sha256sum "$OUTPUT"
