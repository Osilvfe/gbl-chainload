#!/usr/bin/env python3
"""gbl-fastboot-boot-fix.py — patch the GBL FastbootLib boot path.

Patches the base `gbl-chainload.efi` so that `fastboot boot` entered through
GBL's own FastbootLib behaves like the stock ABL path:

  --boot-menu   Skip the ORANGE verified-boot warning menu.  An unlocked
                device always reports ORANGE, so a stock boot image hits this
                menu too.  The menu is the `PRESS POWER KEY TO PAUSE BOOT`
                screen; its wait loop does not advance in the GBL
                `fastboot boot` call context, so without this patch the command
                appears to hang.

  --soc-info    Populate the platform-info getters used by the DTB matcher.
                GBL never initialises `dword_9726C` (BoardSocId),
                `dword_97280` (BoardSocRev) or `dword_97258` (BoardVariant /
                DevInfo DVId); all stay 0, so every vendor_boot blob fails the
                `qcom,msm-id` gate ("No match found for Soc Dtb type") and, once
                that passes, every dtbo entry fails the `qcom,board-id` gate
                ("Unable to find the Board Dtb").  The stock ABL gets the same
                values from its DevInfo protocol; here they are hard-coded to
                the platform's real values (0x294 / 0x20000 / 8).

  --no-dtbo     Do not apply the dtbo overlay to the boot image's DTB, and do
                not advertise `androidboot.dtbo_idx` to the kernel.  This is
                the "GBL fastboot only" branch: normal Android boot through
                the patched ABL is untouched.

Both edits are single AArch64 instructions in `.text` of the GBL EFI.  The PE
has ImageBase 0 and every section maps file offset == RVA, so the constants
below are directly usable as file offsets.

The script refuses to patch anything unless the original instruction word
matches, and it is idempotent (re-running on an already patched file is OK).

Usage:
  scripts/gbl-fastboot-boot-fix.py --in gbl-chainload.efi --out fixed.efi \
      [--boot-menu] [--no-menu-wait] [--soc-info] [--no-dtbo] \
      [--mark bootfix|nodtbo|socfix|logall]
"""

import argparse
import struct
import sys

# --- patch sites (VA == file offset, ImageBase 0) -------------------------

# `sub_3EA54` (load/authenticate boot image), ORANGE branch entry:
#   LDRB W8, [X29,#var_8]
# Replace with an unconditional branch to the shared "continue boot" label.
BOOT_MENU_OFF = 0x3F428
BOOT_MENU_ORIG = 0x394063A8  # LDRB W8, [X29,#0x20+var_8]
BOOT_MENU_PATCH = 0x14000034  # B loc_3F4F8 (+0xD0)

# `ApplyOverlay` (sub_10AB4), right before ufdt_overlay_apply (sub_2681C):
#   BL sub_2681C
# Replace with `MOV X0, X21`: report the unmodified main DTB as the result.
# X21 holds sub_25F00()'s main-FDT copy and is non-NULL on this path.
NO_DTBO_OFF = 0x10B24
NO_DTBO_ORIG = 0x9400573E  # BL sub_2681C
NO_DTBO_PATCH = 0xAA1503E0  # MOV X0, X21

# Fastboot screen build line: "BUILD - gbl-chainload-debug" at 0x77458.
# Replacing the 19-char base name keeps the string length identical.
MARK_OFF = 0x77458
MARK_ORIG = b"BUILD - gbl-chainload-debug"
MARK_BOOTFIX = b"BUILD - gbl-chainload-dbgfx"
MARK_NODTBO = b"BUILD - gbl-chainload-nodtb"
MARK_SOCFIX = b"BUILD - gbl-chainload-socfx"
MARK_LOGALL = b"BUILD - gbl-chainload-logal"

# Platform info getters used by the DTB matcher (sub_128EC):
#   sub_D344 -> (u32) dword_9726C   BoardSocId   / qcom,msm-id low 16 bits
#   sub_D350 -> (u32) dword_97280   BoardSocRev  / qcom,msm-id second cell
# Neither global is ever written anywhere in the GBL image; both are 0 in the
# file.  ABL's own boot log for PLZ110 prints "soc PfId= 0x294 Soc= 0x20000",
# and its matcher uses the same two fields.  Replace each three-instruction
# getter with `MOV W0,#imm; RET`.
SOC_ID_OFF = 0xD344
SOC_ID_ORIG = 0xD0000448  # ADRP X8, #dword_9726C
SOC_ID_LDR_ORIG = 0xB9426D00  # LDR W0, [X8,#dword_9726C]
SOC_ID_RET_ORIG = 0xD65F03C0
SOC_REV_OFF = 0xD350
SOC_REV_ORIG = 0xD0000448  # ADRP X8, #dword_97280
SOC_REV_LDR_ORIG = 0xB9428100  # LDR W0, [X8,#dword_97280]
SOC_REV_RET_ORIG = 0xD65F03C0

DEFAULT_SOC_ID = 0x294
DEFAULT_SOC_REV = 0x20000

# `sub_C9D4` returns dword_97258 (BoardVariant / DevInfo DVId).  The dtbo
# entries carry `qcom,board-id = <8 0>` on this board, and the ABL log prints
# `DVId= 8`.  sub_128EC gates the dtbo match on bit 57 (board variant), so
# without this the overlay search fails with "Unable to find the Board Dtb".
VARIANT_OFF = 0xC9D4
VARIANT_ADRP_ORIG = 0xF0000448  # ADRP X8, #dword_97258
VARIANT_LDR_ORIG = 0xB9425900   # LDR W0, [X8,#dword_97258]
VARIANT_RET_ORIG = 0xD65F03C0
DEFAULT_BOARD_VARIANT = 0x8

# Log-level mask.  Every `sub_3E2C(level, fmt, ...)` call funnels into
# `sub_3EB4`, whose first statement is `if ((sub_38274() & level) == 0) return`.
# `sub_38274` returns `dword_54AF8`, which is 0x80000042 in this build: only
# ERROR (0x80000000), 0x40 and 0x2.  The informational load-address prints in
# `BootLinux` use 0x400000, so they can never appear even in a `--debug` build,
# and no branch patching can force them.  Rewriting the getter to return ~0
# enables every level.  Diagnostics only.
LOG_MASK_OFF = 0x38274
LOG_MASK_ADRP_ORIG = 0x900000E8  # ADRP X8, #dword_54AF8
LOG_MASK_LDR_ORIG = 0xB94AF900   # LDR W0, [X8,#dword_54AF8]
LOG_MASK_RET_ORIG = 0xD65F03C0
LOG_MASK_ALL = 0x12800000        # MOVN W0, #0  (i.e. 0xFFFFFFFF)

# `MenuWait` (sub_4C914): busy-waits `while (qword_AE0B0 != 0) delay(10ms)`.
# qword_AE0B0 is the key-detection timer event; only that event's notify
# clears it. The loop is a pure CNTPCT_EL0 delay and does not pump events.
# In the GBL `fastboot boot` context it was observed not to recover, so every
# menu path that reaches it hangs. Rewriting the prologue to `MOV W0,#1; RET`
# removes the wait unconditionally instead of relying on that timing.
MENU_WAIT_OFF = 0x4C914
MENU_WAIT_ORIG = 0xA9BE7BFD  # STP X29,X30,[SP,#var_20]!
MENU_WAIT_RET_OFF = 0x4C918
MENU_WAIT_RET_ORIG = 0xF9000BF3  # STR X19,[SP,#0x20+var_10]
MENU_WAIT_MOV = 0x52800020      # MOV W0,#1
MENU_WAIT_RET = 0xD65F03C0      # RET

# `UpdateCmdline` (sub_1BD50), writing " androidboot.dtbo_idx=<n>":
#   B.EQ loc_1CA6C
# Replace with an unconditional branch so the property is never emitted.
DTBO_IDX_OFF = 0x1CA1C
DTBO_IDX_ORIG = 0x54000280  # B.EQ loc_1CA6C
DTBO_IDX_PATCH = 0x14000014  # B loc_1CA6C


def die(msg):
    sys.stderr.write(f"gbl-fastboot-boot-fix: error: {msg}\n")
    sys.exit(1)


def read_word(buf, off):
    if off + 4 > len(buf):
        die(f"offset {off:#x} outside file")
    return struct.unpack_from("<I", buf, off)[0]


def patch_word(buf, off, orig, new, label):
    cur = read_word(buf, off)
    if cur == new:
        print(f"  {label}: already applied at {off:#x}")
        return False
    if cur != orig:
        die(f"{label}: unexpected instruction at {off:#x}: "
            f"{cur:#010x} (expected {orig:#010x}); wrong base EFI?")
    struct.pack_into("<I", buf, off, new)
    print(f"  {label}: {off:#x} {orig:#010x} -> {new:#010x}")
    return True


def patch_mark(buf, want):
    cur = bytes(buf[MARK_OFF:MARK_OFF + len(MARK_ORIG)])
    new = {"nodtbo": MARK_NODTBO,
           "socfix": MARK_SOCFIX,
           "logall": MARK_LOGALL}.get(want, MARK_BOOTFIX)
    if cur == new:
        print(f"  mark: already set at {MARK_OFF:#x} ({new.decode()!r})")
        return False
    if cur not in (MARK_ORIG, MARK_BOOTFIX, MARK_NODTBO, MARK_SOCFIX,
                   MARK_LOGALL):
        die(f"mark: unexpected build string at {MARK_OFF:#x}: {cur!r}")
    buf[MARK_OFF:MARK_OFF + len(new)] = new
    print(f"  mark: {MARK_OFF:#x} {cur.decode()!r} -> {new.decode()!r}")
    return True


def movz_w0(imm):
    """Encode `MOVZ W0, #imm` (imm must fit one 16-bit halfword)."""
    for shift in (0, 16):
        if imm >> (shift + 16) == 0 and (imm >> shift) & 0xFFFF:
            return 0x52800000 | (shift // 16) << 21 | (((imm >> shift) & 0xFFFF) << 5)
    if imm == 0:
        return 0x52800000
    die(f"constant {imm:#x} does not fit a single MOVZ")


def patch_soc_info(buf, soc_id, soc_rev, variant):
    changed = False
    changed |= patch_word(buf, SOC_ID_OFF, SOC_ID_ORIG, movz_w0(soc_id),
                          f"soc-info/id={soc_id:#x}")
    changed |= patch_word(buf, SOC_ID_OFF + 4, SOC_ID_LDR_ORIG, 0xD503201F,
                          "soc-info/id-nop")
    changed |= patch_word(buf, SOC_ID_OFF + 8, SOC_ID_RET_ORIG, 0xD65F03C0,
                          "soc-info/id-ret")
    changed |= patch_word(buf, SOC_REV_OFF, SOC_REV_ORIG, movz_w0(soc_rev),
                          f"soc-info/rev={soc_rev:#x}")
    changed |= patch_word(buf, SOC_REV_OFF + 4, SOC_REV_LDR_ORIG, 0xD503201F,
                          "soc-info/rev-nop")
    changed |= patch_word(buf, SOC_REV_OFF + 8, SOC_REV_RET_ORIG, 0xD65F03C0,
                          "soc-info/rev-ret")
    changed |= patch_word(buf, VARIANT_OFF, VARIANT_ADRP_ORIG,
                          movz_w0(variant), f"soc-info/variant={variant:#x}")
    changed |= patch_word(buf, VARIANT_OFF + 4, VARIANT_LDR_ORIG, 0xD503201F,
                          "soc-info/variant-nop")
    changed |= patch_word(buf, VARIANT_OFF + 8, VARIANT_RET_ORIG, 0xD65F03C0,
                          "soc-info/variant-ret")
    return changed


def patch_log_all(buf):
    changed = False
    changed |= patch_word(buf, LOG_MASK_OFF, LOG_MASK_ADRP_ORIG,
                          LOG_MASK_ALL, "log-all/mask")
    changed |= patch_word(buf, LOG_MASK_OFF + 4, LOG_MASK_LDR_ORIG,
                          0xD503201F, "log-all/nop-ldr")
    changed |= patch_word(buf, LOG_MASK_OFF + 8, LOG_MASK_RET_ORIG,
                          0xD65F03C0, "log-all/ret")
    return changed


def sanity_check_pe(buf):
    if len(buf) < 0x40 or buf[:2] != b"MZ":
        die("input is not a PE image (missing MZ)")
    pe = struct.unpack_from("<I", buf, 0x3C)[0]
    if pe + 0x18 > len(buf) or buf[pe:pe + 4] != b"PE\0\0":
        die("input is not a PE image (missing PE header)")
    magic = struct.unpack_from("<H", buf, pe + 24)[0]
    if magic != 0x20B:
        die(f"expected PE32+ (0x20b), got {magic:#x}")
    image_base = struct.unpack_from("<Q", buf, pe + 24 + 24)[0]
    if image_base != 0:
        die(f"expected ImageBase 0 (VA == file offset), got {image_base:#x}")


def main():
    ap = argparse.ArgumentParser(
        prog="gbl-fastboot-boot-fix.py",
        description="Patch GBL FastbootLib boot-path behaviour.")
    ap.add_argument("--in", dest="inp", required=True, help="input EFI")
    ap.add_argument("--out", dest="out", required=True, help="output EFI")
    ap.add_argument("--boot-menu", action="store_true",
                    help="skip the ORANGE verified-boot pause-boot menu")
    ap.add_argument("--soc-info", action="store_true",
                    help="hard-code the DTB matcher's BoardSocId/BoardSocRev "
                         "(GBL never initialises them)")
    ap.add_argument("--soc-id", type=lambda v: int(v, 0), default=DEFAULT_SOC_ID,
                    help="BoardSocId for --soc-info (default: 0x294)")
    ap.add_argument("--soc-rev", type=lambda v: int(v, 0), default=DEFAULT_SOC_REV,
                    help="BoardSocRev for --soc-info (default: 0x20000)")
    ap.add_argument("--board-variant", type=lambda v: int(v, 0),
                    default=DEFAULT_BOARD_VARIANT,
                    help="BoardVariant/DVId for --soc-info (default: 8)")
    ap.add_argument("--no-dtbo", action="store_true",
                    help="do not apply the dtbo overlay and drop dtbo_idx")
    ap.add_argument("--no-menu-wait", action="store_true",
                    help="make the menu wait loop (sub_4C914) return "
                         "immediately instead of waiting on the key event")
    ap.add_argument("--mark",
                    choices=("bootfix", "nodtbo", "socfix", "logall"),
                    help="change the Fastboot screen BUILD line so the running "
                         "payload can be identified")
    ap.add_argument("--log-all", action="store_true",
                    help="make the firmware log mask accept every level "
                         "(enables BootLinux's Kernel/Ramdisk/DTB address "
                         "prints); diagnostics only")
    ap.add_argument("--check", action="store_true",
                    help="only verify that the known instruction words exist")
    args = ap.parse_args()

    if not (args.boot_menu or args.no_dtbo or args.no_menu_wait
            or args.soc_info or args.log_all or args.mark or args.check):
        die("nothing to do: pass --boot-menu, --no-dtbo, --mark, or --check")

    with open(args.inp, "rb") as f:
        buf = bytearray(f.read())
    sanity_check_pe(buf)

    print(f"input: {args.inp} ({len(buf)} bytes)")
    changed = False
    if args.boot_menu:
        changed |= patch_word(buf, BOOT_MENU_OFF, BOOT_MENU_ORIG,
                              BOOT_MENU_PATCH, "boot-menu")
    if args.no_dtbo:
        changed |= patch_word(buf, NO_DTBO_OFF, NO_DTBO_ORIG,
                              NO_DTBO_PATCH, "no-dtbo/apply")
        changed |= patch_word(buf, DTBO_IDX_OFF, DTBO_IDX_ORIG,
                              DTBO_IDX_PATCH, "no-dtbo/dtbo_idx")

    if args.soc_info:
        changed |= patch_soc_info(buf, args.soc_id, args.soc_rev,
                                  args.board_variant)

    if args.log_all:
        changed |= patch_log_all(buf)

    if args.no_menu_wait:
        changed |= patch_word(buf, MENU_WAIT_OFF, MENU_WAIT_ORIG,
                              MENU_WAIT_MOV, "no-menu-wait/mov")
        changed |= patch_word(buf, MENU_WAIT_RET_OFF, MENU_WAIT_RET_ORIG,
                              MENU_WAIT_RET, "no-menu-wait/ret")

    if args.mark:
        changed |= patch_mark(buf, args.mark)

    if args.check:
        print("check: instruction words match the expected base EFI")
        return

    with open(args.out, "wb") as f:
        f.write(buf)
    print(f"wrote: {args.out} ({len(buf)} bytes, "
          f"{'modified' if changed else 'unchanged'})")


if __name__ == "__main__":
    main()
