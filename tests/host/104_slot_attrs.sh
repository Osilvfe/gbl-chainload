#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=tests/host/.last/slot-attrs
mkdir -p "$out"
"${CC:-cc}" -Wall -Wextra -Werror -Wno-unused-variable -std=c99 -O1 -g -fshort-wchar \
  -DMDEPKG_NDEBUG -D__FORTIFY_SOURCE=1 -DDISABLE_NEW_DEPRECATED_INTERFACES \
  -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Iedk2/MdePkg/Include -Iedk2/MdePkg/Include/X64 \
  -Iedk2/QcomModulePkg/Include -Iedk2/QcomModulePkg/Include/Library -Iedk2/QcomModulePkg/Library \
  tests/host/helpers/test_slot_attrs.c -o "$out/test_slot_attrs"
"$out/test_slot_attrs"
