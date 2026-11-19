#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out=tests/host/.last/usb-msc
mkdir -p "$out"
# Build matrix: the default image, a partition-export image, and the two write
# opt-ins. The write path must be compiled out unless it was asked for.
for variant in 0:0:0 1:0:0 1:1:0 1:1:1; do
  IFS=: read -r partition_export write partwrite <<<"$variant"
  binary="$out/test_usb_msc_$partition_export$write$partwrite"
  extra_flags=()
  if [[ "$partition_export" -eq 1 ]]; then
    extra_flags+=(-DGBL_USB_MSC_PARTITION_EXPORT=1)
  fi
  if [[ "$write" -eq 1 ]]; then
    extra_flags+=(-DGBL_USB_MSC_WRITE=1)
  fi
  if [[ "$partwrite" -eq 1 ]]; then
    extra_flags+=(-DGBL_USB_MSC_WRITE_PARTITIONS=1)
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
"$out/test_usb_msc_000" "$out/ram-disk.img"
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
for forbidden in ("->Reset (", "UpdatePartitionEntries", "SetActiveSlot"):
    assert forbidden not in media, forbidden
# Block writes exist only inside an opt-in region, so a normal build has no
# path from the host to real storage.
regions = []
position = media.find("#if GBL_USB_MSC_WRITE_PARTITIONS")
while position != -1:
    regions.append((position, media.index("#endif", position)))
    position = media.find("#if GBL_USB_MSC_WRITE_PARTITIONS", position + 1)
assert regions, "no GBL_USB_MSC_WRITE_PARTITIONS guard found"
assert media.index("PartitionWrite (") > regions[0][0]
for token in ("->WriteBlocks", "->FlushBlocks"):
    position = media.find(token)
    while position != -1:
        assert any(start < position < end for start, end in regions), (token, position)
        position = media.find(token, position + 1)
# The RAM backend is memory-only: no BlockIo, and its write path is compiled in
# only behind the write flag.
ram_start = media.index("OpenRamMedia (")
ram_end = media.index("CheckMedia (IN MSC_PARTITION *Partition)")
assert "BlockIo" not in media[ram_start:ram_end]
assert "#if GBL_USB_MSC_WRITE" in media
# A writable medium is flushed when it is released.
assert "Media->Flush (Media->Context)" in media
# The file/ISO backend must stay read-only: open for read and only Read/Close.
assert "gEfiSimpleFileSystemProtocolGuid" in media
assert "EFI_FILE_MODE_READ" in media
for forbidden in ("->Write (", "->SetInfo (", "->Delete (", "EFI_FILE_MODE_WRITE"):
    assert forbidden not in media, forbidden
scsi = (root / "GblChainloadPkg/Application/GblChainload/UsbMscScsi.c").read_text()
assert "GBL_MSC_MODE_PAGE_CACHING" in scsi and "GBL_MSC_MODE_PAGE_CONTROL" in scsi
assert "GBL_MSC_PERIPHERAL_CDROM" in scsi
assert "GBL_MSC_MODE_PAGE_RECOVERY" in scsi and "GBL_MSC_MODE_PAGE_INFORMATIONAL" in scsi
assert "ModeSelectValidate" in scsi and "GblMscBotFlush" in scsi
print("ok 103_usb_msc — default partition-export guard, read-only ISO/file backend with an opt-in write path, MODE SENSE pages, protocol tests, sanitizers and reproducible Fastboot integration patch")
PY
