#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=tests/host/.last/usb-msc
mkdir -p "$out"
for partition_export in 0 1; do
  binary="$out/test_usb_msc_$partition_export"
  extra_flags=()
  if [[ "$partition_export" -eq 1 ]]; then
    extra_flags+=(-DGBL_USB_MSC_PARTITION_EXPORT=1)
  fi
  "${CC:-cc}" -Wall -Wextra -Werror -std=c99 -O1 -g -fshort-wchar \
    -DMDEPKG_NDEBUG -D__FORTIFY_SOURCE=1 -DDISABLE_NEW_DEPRECATED_INTERFACES \
    "${extra_flags[@]}" \
    -fsanitize=address,undefined -fno-omit-frame-pointer \
    -Iedk2/MdePkg/Include -Iedk2/MdePkg/Include/X64 -Iedk2/QcomModulePkg/Include \
    tests/host/helpers/test_usb_msc.c \
    GblChainloadPkg/Application/GblChainload/UsbMscScsi.c \
    GblChainloadPkg/Application/GblChainload/UsbMscMedia.c \
    -o "$binary"
  "$binary"
done
"$out/test_usb_msc_0" "$out/ram-disk.img"
if command -v fsck.fat >/dev/null 2>&1; then
  fsck.fat -n "$out/ram-disk.img"
fi
python3 - <<'PY'
from pathlib import Path
import subprocess
import tempfile

root = Path.cwd()
patch = root / "scripts/edk2/gbl-usb-msc.patch"
source = "QcomModulePkg/Library/FastbootLib/FastbootMain.c"
with tempfile.TemporaryDirectory(prefix="gbl-msc-hook-") as temporary:
    workspace = Path(temporary)
    destination = workspace / source
    destination.parent.mkdir(parents=True)
    destination.write_bytes(subprocess.check_output(["git", "-C", "edk2", "show", "HEAD:" + source]))
    destination.chmod(0o755)
    subprocess.run(["git", "apply", "--check", str(patch)], cwd=workspace, check=True)
    subprocess.run(["git", "apply", str(patch)], cwd=workspace, check=True)
    subprocess.run(["git", "apply", "--reverse", "--check", str(patch)], cwd=workspace, check=True)
    content = destination.read_text()
    switch = content.index("if (GblUsbMscRequested ())")
    stop = content.index("Fbd.UsbDeviceProtocol->Stop ()", switch)
    run = content.index("GblUsbMscRun (&Released)", stop)
    released = content.index("if (!Released)", run)
    resume = content.index("Fbd.UsbDeviceProtocol->StartEx (&DescSet)", released)
    assert stop < run < released < resume
    assert "GblFastbootEscapePending = FALSE;" in content[switch:stop]
    assert "GblFastbootEspBootPending = FALSE;" in content[switch:stop]
    assert "GblUsbMscRegisterCommands ();" in content
    assert "GblUsbMscCancel ();" in content[stop:run]
media = (root / "GblChainloadPkg/Application/GblChainload/UsbMscMedia.c").read_text()
assert "->ReadBlocks (" in media
for forbidden in ("->WriteBlocks", "->FlushBlocks", "->Reset (", "UpdatePartitionEntries", "SetActiveSlot"):
    assert forbidden not in media, forbidden
print("ok 103_usb_msc — default partition-export guard, protocol tests, sanitizers and reproducible Fastboot integration patch")
PY
