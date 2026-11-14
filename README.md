# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 shim is usable for mode-0 and mode-1. Project documentation, reverse-engineering findings, and the current milestone marker live in [`docs/project/`](docs/project/).

Working artifacts: `dist/mode-0.efi` (unlocked observation + universal preservation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).

Mode-1 supports the "stock recovery + custom system" use case by default. Custom recovery + normal boot requires a disk-side graft of stock vbmeta — see [`docs/project/next-milestone.md`](docs/project/next-milestone.md). Both a host script and a device-side companion module remain next-milestone work; neither ships today.

## Modes

- **mode-0** — unlocked observation + universal preservation build. Installs protocol hooks for logging and for the narrow preservation baseline: drop TZ soft-fuse advancement and swallow `oplusreserve1` / `opporeserve1` writes. VB lock-state and OplusSec writes pass through so stock ABL can run the real relock procedure.
- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view. The fakelock is **ephemeral**: it never persists a locked state to RPMB. Four lock-state RPMB paths are suppressed under fakelock — VB `WRITE_CONFIG`, VB device reset, OplusSec `write_rpmb_boot_info` (0x0A), and the KeyMaster device-state commit `WRITE_KM_DEVICE_STATE` (0x203, OEM-added on sm8845/macan). The last one is what otherwise lets a spoofed locked RoT reach RPMB and disagree with the real bootloader after reverting to stock. See `docs/project/re-findings.md` → "Device: macan / sm8845".
- **mode-2** — TA-payload spoof at QSEE/SPSS boundaries (custom-ROM mode); ABL stays honest; per-OTA typed-struct profile injected via `GblPayloadLib`.

## Build

```bash
./scripts/build.sh --mode 0               # unlocked observation + preservation baseline
./scripts/build.sh --mode 1               # fakelock production silent
./scripts/build.sh --mode 1 --auto --debug --verbose   # fakelock dev capture
./scripts/build.sh --mode 2               # TA-payload spoof (custom-ROM mode)
```

## Logging

Three primitives from `GblChainloadPkg/Include/Library/GblLog.h` (ASCII format strings, drop the `L` prefix):

| Macro / call | Build flag | Goes to screen? | Goes to UefiLog<N>.txt? |
|--------------|------------|------------------|--------------------------|
| `Print(L"...")` | always | yes | yes |
| `GBL_INFO("...")` | `GBL_DEBUG=0` (prod) | no | yes (via `ReportStatusCode` → UART) |
| `GBL_INFO("...")` | `GBL_DEBUG=1` (`--debug`) | yes (via AsciiPrint) | yes |
| `VERBOSE("...")` | `GBL_VERBOSE=0` | — (compile-stripped to no-op) | — |
| `VERBOSE("...")` | `GBL_VERBOSE=1` (`--verbose`) | yes (via AsciiPrint) | yes |

`DEBUG((DEBUG_ERROR, "..."))` and `DEBUG((DEBUG_WARN, "..."))` are kept for unconditional error / warning paths (route through the same `ReportStatusCode` path).

### When to use which

- **`Print(L"...")`** — content the user MUST see regardless of build: user prompts ("Hold VolUp within 3s..."), fatal error messages ("LOGFS PARTITION NOT FOUND"). Screen-visible, also lands in UefiLog. Use sparingly.

- **`GBL_INFO("...")`** — semantic events relevant to verifying correctness: mutations our hooks perform (`vb-fakelock | is_unlocked 1->0`), swallows (`vb-rwstate | swallowed`), crypto outputs on intercepted commands (`qsee-km | SET_ROT | rotDigest=...`, `spss-bootstate | unlocked=0 | color=0`), install banners, BootFlow status. Silent on prod screen, visible under `--debug`, always in UefiLog. **This is the "did the hook do what I expect?" channel.**

- **`VERBOSE("...")`** — raw pass-through traces: every qsee call, every scm syscall, per-call hex dumps, scan-loop iterations. Compile-stripped from prod and `--debug` builds; only present in `--verbose`. **Don't use it for anything you'd want to see in prod.**

### Classification rules

When adding a new emit in a hook or patch:

- Is it a **mutation** the hook performs, or a **swallow** the hook executes, or a **crypto output on an intercepted command**? → `GBL_INFO`.
- Is it a pass-through query (READ / GET / probe) or a per-call raw dump? → `VERBOSE`.
- Is it protocol-internal marshalling (Mink op decoding, vtable dispatch tracing) with no semantic payload? → **don't emit at all**. Both prod and `--verbose` lose nothing.
- Is it a user prompt or an error that MUST be visible? → `Print(L"...")`.

### Format string gotchas

- All `GBL_INFO` / `VERBOSE` format strings are CHAR8 ASCII (`"foo=%u\n"`, no `L` prefix).
- `%s` consumes CHAR8\* in `GBL_INFO` / `VERBOSE` / `AsciiPrint`. To print a CHAR16\* string, convert it to ASCII first via `UnicodeStrToAsciiStrS` into a local buffer.
- `%a` always means ASCII string regardless of macro choice.

## Repo conventions

- `GblChainloadPkg/Library/DynamicPatchLib/{universal,oem,mode_1}/` — patches scoped by applicability.
- `GblChainloadPkg/Library/ProtocolHookLib/UniversalBaseline.c` — narrow policies every mode ships.
- `GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.c` — mode-1-specific hooks atop baseline.

## FastbootLib surface

The bundled edk2 FastbootLib is trimmed for RAM-loaded gbl-chainload testing and scripts.

Useful getvars:

- `gbl-chainload_mode` — `mode-0`, `mode-1`, etc.
- `gbl-chainload_date`
- `gbl-chainload_auto`
- `gbl-chainload_debug`
- `gbl-chainload_verbose`
- `unlocked`
- `oem-unlock-allowed`
- `vbmeta:capabilities`
- `vbmeta:slot`
- `vbmeta:warning`
- `vbmeta:<partition>:status`
- `vbmeta:<partition>:descriptor-type`

Useful commands:

- `fastboot stage <file>` — stage an image for `boot`, `oem boot-efi`, or other staged-data commands.
- `fastboot oem escape` — leave gbl-chainload FastbootLib and continue into patched ABL.
- `fastboot oem boot-efi` — boot the currently staged EFI image with `LoadImage()` / `StartImage()`.
- `fastboot oem oem-unlock-toggle` — enable the FRP OEM-unlock-allowed bit; second use is a no-op.
- `fastboot flashing lock` / `fastboot flashing unlock` — update DevInfo lock state while skipping forced recovery wipe.

Fastboot screen additions:

- `DATE - ...` build timestamp line.
- `OEM UNLOCK ALLOWED - yes/no` state line.
- `AVB WARNING - ...` warning line when lightweight vbmeta probing detects a risky state.
- `Enable OEM unlock` menu action.
- `Escape` menu action.

There is also an untested `Boot ESP` menu option intended for directly booting operating systems from USB.

## Mode-0 reserve preservation test plan

Use only RAM-load testing for gbl-chainload itself:

1. Boot/install the mode-0 chainload path.
2. Install stock firmware.
3. Perform a relock through the stock bootloader flow.
4. Unlock again and pull logfs.
5. Confirm a reserve write swallow was logged for the token block, e.g.
   `blockio | op=write-swallow | reason=token-zero-write | p=oplusreserve1`.

The token-zero intercept is also printed as vital user-visible output in every
build:

`GBL: intercepted reserve token zeroing on oplusreserve1 LBA 1114; token preserved`

That line appears when the relock path attempts to zero the token block.

## Release convention

Development and CI use `main`. Releases use an existing `release-X.Y.Z` tag
matching `VERSION`; the release workflow checks and builds that exact commit,
then creates a draft for review. Manual dispatch takes the same existing tag
and does not create one. Historical `v...` tags and b4 checkpoints remain
available, but do not trigger new releases. Artifact filenames and changelog
version headings retain their existing version format.

## GBL FastbootLib boot fixes

`fastboot boot` entered through GBL's own FastbootLib (VolUp) differs from the
patched ABL path (VolDown): the GBL build has no patch7/patch10. An unlocked
device always reports the `ORANGE` verified-boot state, independent of the boot
image's signature, so even a stock boot image hits GBL's unlocked warning menu
(`PRESS POWER KEY TO PAUSE BOOT`). In this call context the menu's wait loop
busy-waits on the key-detection event instead of pumping it, and observed
behaviour is that `fastboot boot` does not proceed, for stock and non-stock
images alike.

`scripts/gbl-fastboot-boot-fix.py` applies byte-validated edits:

```bash
scripts/gbl-fastboot-boot-fix.py --in gbl-chainload-debug.efi \
    --out gbl-chainload-debug-socfix.efi \
    --boot-menu --no-menu-wait --soc-info
scripts/gbl-fastboot-boot-fix.py --in gbl-chainload-debug.efi \
    --out gbl-chainload-debug-bootfix-nodtbo.efi --boot-menu --no-dtbo
```

- `--boot-menu` — skip the ORANGE warning/pause menu and continue booting.
- `--soc-info` — hard-code the DTB matcher's platform values: `BoardSocId`
  (`0x294`), `BoardSocRev` (`0x20000`) and board variant (`8`) on fairlady.
  GBL never initialises the `.data` globals behind `sub_D344`, `sub_D350` and
  `sub_C9D4`; left at 0 every `vendor_boot` blob fails the `qcom,msm-id` gate
  (`No match found for Soc Dtb type`) and every dtbo entry fails the
  `qcom,board-id` gate (`Unable to find the Board Dtb`). `--soc-id`,
  `--soc-rev` and `--board-variant` override them for other boards.
- `--no-dtbo` — return the main DTB without applying the dtbo overlay, and stop
  emitting `androidboot.dtbo_idx`. `androidboot.dtb_idx` is still emitted, and
  the base DTB still comes from `vendor_boot`; this branch only removes the
  dtbo overlay. It does not bypass AVB.
- `--no-menu-wait` — make `sub_4C914`, the menu wait loop, return immediately.
  That loop is `while (qword_AE0B0 != 0) delay(10ms)`; `qword_AE0B0` is the
  key-detection timer event, cleared only by its own notify, and the loop is a
  pure counter delay that does not pump events. In the GBL `fastboot boot`
  context it was observed not to recover, so any menu path reaching it hangs.
  This edit removes the wait outright rather than relying on that timing.
- `--log-all` — make the firmware log mask accept every level. Every
  `sub_3E2C(level, ...)` call ends in `sub_3EB4`, which returns early unless
  `(sub_38274() & level) != 0`; `sub_38274` reads `dword_54AF8`, which is
  `0x80000042` in this build. `BootLinux`'s load-address prints use `0x400000`,
  so they are unreachable regardless of build flags or branch patching; this
  option rewrites the getter to return `~0`. Diagnostics only — it floods the
  64 KiB log ring, so read the rotated `UefiLogSaved*.txt` files.
- `--mark bootfix|nodtbo|socfix|logall` — rewrite the Fastboot screen `BUILD - ...`
  line so the running payload can be identified (`gbl-chainload-dbgfx` /
  `gbl-chainload-nodtb` / `gbl-chainload-socfx`). Use it to confirm which EFI
  is actually live. `--mark logall` prints `gbl-chainload-logal`.

The script checks the expected instruction words, is idempotent, and refuses an
unknown base EFI. It is scoped to the GBL EFI: normal Android boot through the
patched ABL is unaffected.

`--no-dtbo` in the byte-patch script was historically a **variant of the EFI**.
Source builds expose it as a separate device-side command, so one EFI covers
both paths. The fastboot wire protocol gives `oem ...` no data phase, so the
image is uploaded with `stage` first:

```bash
fastboot stage boot.img
fastboot oem boot-nodtbo
```

`oem boot-nodtbo` boots the staged payload without loading the dtbo partition
or applying its overlay, and without advertising `androidboot.dtbo_idx`. The
base DTB still comes from the boot/vendor_boot image, and AVB is still run.
Plain `fastboot boot <image>` keeps the dtbo overlay.

### Boot trace on logfs (`\GblTrace.txt`)

`UefiLog1.txt` is written by the platform status-code handler and only reaches
logfs at "write final log", immediately before `ExitBootServices`. A boot that
hangs earlier leaves **no record at all**. `LogFsLib` therefore appends one line
per milestone to `\GblTrace.txt` and flushes it immediately:

```text
    7627 entry: build=gbl-chainload-debug boardInit=Success chip=0x294 ver=0x20000 plat=8
    8142 enter: fastbootlib
   14062 cmd: boot size=0x2C6F000 skipdtbo=1
   14107 cmd: avb status=Success hdr=2 imgs=2 state=1
   14109 bootlinux: pname=boot_b skipdtbo=1
   14225 dtb: hdr=2 img=0x2C6ED3B off=0x2C53000 skip=1 dtbo=0
   14228 dtb: appended-overlay=Success tot=0x1BD3B room=0x201000
   14231 ram: read=Success n=11
   14232 ram: update=Success upd=18
   14262 cmdline: status=Success len=2
   14431 dt-update: status=Success
   14436 pre-ebs
```

Each line is prefixed with the firmware uptime in milliseconds, which tells a
hang apart from a watchdog reset. The trace handle is closed just before
`ExitBootServices`, so no open FAT file is left behind at handoff.

The file is deleted on every `LogFsInit`, so it always describes the most
recent boot, and it is capped at 48 KiB. Read it from Android with:

```bash
su -c "mkdir -p /data/local/tmp/logfs && mount -t vfat /dev/block/by-name/logfs /data/local/tmp/logfs"
adb pull /data/local/tmp/logfs/GblTrace.txt
```

### Building the fastboot-boot fix from source (native, no Docker)

`scripts/build-native.sh` builds the debug GBL directly on the host and emits
`dist/gbl-chainload-debug-gcc5.efi`:

```bash
scripts/build-native.sh
# optional: scripts/build-native.sh --release --output dist/gbl-chainload.efi
```

The script creates a local GCC wrapper (`-std=gnu17 -Wno-error`) and a private
EDK2 tools mirror whose `GccBase.lds` drops `.eh_frame`/`.eh_frame_hdr`; both
are required with current binutils/GCC and do not modify the `edk2` submodule.
The EDK2-side changes live in `scripts/edk2/gbl-edk2-fixes.patch` and are
applied idempotently on every run. They keep AVB verification intact but mark
`fastboot boot` with
`BootInfo.SkipVerifiedBootUi`, so the ORANGE warning menu and its key-event
wait are skipped. `GblChainloadEntry` calls `BoardInit()` after loading the GBL
FV, which populates the same platform globals LinuxLoader uses instead of
hard-coding one board's `msm-id`/`board-id`. The same patch adds
`BootInfo.SkipDtboOverlay` and the `oem boot-nodtbo` command, which skips
`LoadAndValidateDtboImg`, the board-dtbo overlay, and `androidboot.dtbo_idx`.
The same patch fixes the `LibUfdt` overlay bump allocator: the old code kept
its offset in a `static int` that was never reset and left a freed buffer
pointer behind, so a second overlay in the same boot either allocated from a
stale offset or double-freed.
