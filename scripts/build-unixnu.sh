#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT"
TARGET=RELEASE
while [[ $# -gt 0 ]]; do
  case "$1" in
    --debug) TARGET=DEBUG; shift ;;
    -h|--help)
      echo "Usage: scripts/build-unixnu.sh [--debug]"
      echo "Build pinned UniXNU and DarwinBoot sources with the GBL edk2 core."
      echo "Outputs: Build/unixnu/<RELEASE|DEBUG>/{UniXNU,DarwinBoot}.efi"
      exit 0 ;;
    *) echo "unknown flag: $1" >&2; exit 2 ;;
  esac
done

for tool in git python3 make; do
  command -v "$tool" >/dev/null || { echo "missing build tool: $tool" >&2; exit 1; }
done

mapfile -t SOURCE_LOCK < <(python3 - <<'PY'
import json
from pathlib import Path

sources = json.loads(Path("scripts/unixnu-sources.json").read_text())
for name in ("unixnu", "darwinboot"):
    print(sources[name]["url"])
    print(sources[name]["revision"])
PY
)
[[ ${#SOURCE_LOCK[@]} -eq 4 ]] || { echo "invalid UniXNU source lock" >&2; exit 1; }

prepare_source() {
  local source_url="$1" revision="$2" source_dir="$3" sparse_path="$4"
  [[ "$revision" =~ ^[0-9a-f]{40}$ ]] || { echo "invalid source revision" >&2; exit 1; }
  if [[ ! -d "$source_dir/.git" ]]; then
    git init --quiet "$source_dir"
    git -C "$source_dir" remote add origin "$source_url"
  fi
  if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
    echo "source cache has local changes: $source_dir" >&2
    exit 1
  fi
  if ! git -C "$source_dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
    git -C "$source_dir" fetch --depth 1 --filter=blob:none "$source_url" "$revision"
  fi
  if [[ -n "$sparse_path" ]]; then
    git -C "$source_dir" sparse-checkout set "$sparse_path"
  fi
  git -C "$source_dir" checkout --quiet --detach "$revision"
}

UNIXNU_SOURCE="$REPO_ROOT/Build/unixnu/source"
DARWINBOOT_SOURCE="$REPO_ROOT/Build/unixnu/darwinboot"
prepare_source "${SOURCE_LOCK[0]}" "${SOURCE_LOCK[1]}" "$UNIXNU_SOURCE" package
prepare_source "${SOURCE_LOCK[2]}" "${SOURCE_LOCK[3]}" "$DARWINBOOT_SOURCE" ""
if [[ ! -f edk2/MdePkg/Library/BaseFdtLib/libfdt/libfdt/fdt.c ]]; then
  git -C edk2 submodule update --init -- MdePkg/Library/BaseFdtLib/libfdt
fi
scripts/build-native.sh --prepare-only

export WORKSPACE="$REPO_ROOT"
export PACKAGES_PATH="$REPO_ROOT:$UNIXNU_SOURCE/package:$DARWINBOOT_SOURCE:$REPO_ROOT/edk2"
export EDK_TOOLS_PATH="$REPO_ROOT/Build/native-edk-tools"
export CONF_PATH="$REPO_ROOT/Build/Conf"
export GCC5_AARCH64_PREFIX="$REPO_ROOT/Build/native-toolchain/bin/aarch64-linux-gnu-"
export PYTHON_COMMAND=python3
export PATH="$REPO_ROOT/edk2/BaseTools/BinWrappers/PosixLike:$REPO_ROOT/edk2/BaseTools/Source/C/bin:$PATH"
build -p GblChainloadPkg/Unixnu.dsc -a AARCH64 -b "$TARGET" -t GCC5 -n "$(getconf _NPROCESSORS_ONLN)"

OUTPUT_DIR="$REPO_ROOT/Build/unixnu/$TARGET"
mkdir -p "$OUTPUT_DIR"
for component in UniXNU DarwinBoot; do
  cp "Build/UnixnuClo/${TARGET}_GCC5/AARCH64/$component.efi" "$OUTPUT_DIR/"
done
sha256sum "$OUTPUT_DIR/UniXNU.efi" "$OUTPUT_DIR/DarwinBoot.efi"
