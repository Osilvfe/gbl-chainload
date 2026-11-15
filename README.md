# gbl-chainload

EFI System Partition (EFISP) chainloader for OnePlus/Oppo devices using Qualcomm's GBL/EFISP load mechanism. Patches the active-slot ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL.

## Status

v2 shim is usable for mode-0 and mode-1. Project documentation, reverse-engineering findings, and the current milestone marker live in [`docs/project/`](docs/project/).

Working artifacts: `dist/mode-0.efi` (unlocked observation + universal preservation build) and `dist/mode-1.efi` (protocol-hook fakelock via `QCOM_VERIFIEDBOOT_PROTOCOL` mutation; KM/Oplus see locked/green when stock images verify cleanly).

Mode-1 supports the "stock recovery + custom system" use case by default. Custom recovery + normal boot requires a disk-side graft of stock vbmeta — see [`docs/project/next-milestone.md`](docs/project/next-milestone.md). Both a host script and a device-side companion module remain next-milestone work; neither ships today.

## Modes

- **mode-0** — unlocked observation + universal preservation build. Installs protocol hooks for logging and for the narrow preservation baseline: drop TZ soft-fuse advancement and swallow `oplusreserve1` / `opporeserve1` writes. VB lock-state and OplusSec writes pass through so stock ABL can run the real relock procedure.
- **mode-1** — protocol-hook fakelock. ABL sees locked DeviceInfo and builds KM SET_ROT/SET_BOOT_STATE off that view. The fakelock is **ephemeral**: it never persists a locked state to RPMB. Four lock-state RPMB paths are suppressed under fakelock — VB `WRITE_CONFIG`, VB device reset, OplusSec `write_rpmb_boot_info` (0x0A), and the KeyMaster device-state commit `WRITE_KM_DEVICE_STATE` (0x203). `0x203` is OEM-side behaviour, not a per-device quirk: the open BSP has only the enum, and OPPO's ABL issues it on the sm8850 family (observed on fairlady every boot). The last one is what otherwise lets a spoofed locked RoT reach RPMB and disagree with the real bootloader after reverting to stock. See `docs/project/re-findings.md` → "Device: macan / sm8845".
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

## Slot flags: `gbl slot` + KernelSU harness

The A/B slot attributes live in the GPT entries of `boot_a`/`boot_b`, not in
`misc` (see `docs/project/re-findings.md`, "Where the flag actually lives"):

```text
entry + 0x30, UINT64
  bits 48..49  priority
  bit  50      active
  bits 51..53  max retry count
  bit  54      successful
  bit  55      unbootable
```

`gbl slot` reads and rewrites that word. Like ABL's
`UpdatePartitionAttributes()`, it stores the same table to **both** the primary
and the backup GPT and recomputes the entry-array CRC and the header CRC of
each, so a write is equivalent to what ABL does on `fastboot set_active`.

```sh
gbl slot show                        # on device: local GPT; on host: fastboot getvar
gbl slot show --fastboot             # read-only over fastboot (no root)
gbl slot show --adb                  # run the module binary on the phone (all fields)
gbl slot show --disk /dev/block/sda  # local node or an image file
gbl slot set --slot b --unbootable 0 --dry-run
gbl slot set --slot b --unbootable 0 --adb
gbl slot backup --out gpt.bin               # local
gbl slot backup --adb --out gpt.bin         # dump on the phone, adb pull
gbl slot restore --in gpt.bin --adb
```

On a host there is no `/dev/block` for the phone, so `gbl slot show` falls back
to `fastboot getvar` automatically. ABL publishes `slot-unbootable:{a,b}`,
`slot-successful:{a,b}`, `slot-retry-count:{a,b}` and `current-slot`
unconditionally (`FastbootCmds.c:466 FastbootPublishSlotVars`), but **not**
priority/active — use `--adb` when you need every field. Writing is only
possible through `--adb` (or the on-device binary): fastboot has no command
that sets these bits. Both remote paths probe `fastboot devices` / `adb devices`
first, so they never hang waiting for a device.

The disk is resolved from `/dev/block/by-name/boot_a` (partition suffix
stripped: `sda17`→`sda`, `mmcblk0p12`→`mmcblk0`), else by scanning
`/dev/block`; `--disk` overrides it, `--lba` pins the sector size instead of
probing 512/4096. Refusals are deliberate:

- a GPT header or entry array whose CRC does not validate is never written;
- both `boot_a` and `boot_b` must exist;
- making **both** slots unbootable needs `--force` — that is the state stock
  ABL cannot leave without fastboot.

`cargo test -p gbl` covers the parser/writer against a synthetic 512- and
4096-byte-sector GPT (round-trip, reseal, corrupt-table rejection).

The same binary is shipped in a KernelSU module with a WebUI, so the flag can
be flipped from the phone without a boot failure:

```sh
./tools/slotflag/build.sh     # cross-builds gbl for aarch64 -> dist/gbl-slotflag-v1.0.zip
```

The module ships `bin/gbl` (the same multicall binary) plus `bin/slotctl`, a
small shell wrapper that pins the paths and creates
`/data/adb/gbl-slotflag/backup` on demand. The WebUI only talks to `slotctl`:

```sh
slotctl state                 # key=value dump used by the page
slotctl set --slot b --unbootable 1
slotctl backup                # -> path=/data/adb/gbl-slotflag/backup/gpt-*.bin
slotctl backups               # newest first
slotctl restore [FILE]        # default: newest backup
```

The page serialises `ksu.exec` calls, treats a non-zero `errno` as failure,
keeps the action result visible across a refresh, and disables the controls
while a write is in flight. It is covered by a headless test that loads the
real page in jsdom against a fake `slotctl`:

```sh
npm install jsdom
node tools/slotflag/webui-test.js
``` It leaves `webroot/` permissions to the manager
(KernelSU sets mode + SELinux context there). `fastboot getvar
slot-unbootable:a` reads the same bit for a host-side cross-check.

## ABL patch12 v2 / patch13 (fairlady)

Two host-side `CACHED_ABL` patches cover the OPPO failure paths that turn a
recoverable device into a black brick.

**patch12 v4 — orange branch disabled (all entries).** The fastboot entry
(`LinuxLoaderEntry`, PE `0x4E54` on PLZ110 16.0.5.701) can lose the device in
three places:

```c
if (fastboot_requested) {
    if (olock_secure_lock) goto REFUSE;      /* 0x6CD8 CBZ W26 */
LABEL_341:
    if (no_memcard_protocol) goto SPIN;      /* 0x6D64 TBNZ X19,#63 */
LABEL_342:
    print "Launching fastboot";              /* 0x6D68 */
    if (fastboot_unlock_verify() != 0) {
        print "...error and reboot.";
        ResetSystem(0x3E);                   /* 0x6D94 */
    }
} else if (LoadImageAndAuth() != 0) {
    if (olock_secure_lock) goto REFUSE;      /* 0x6D60 CBNZ W26 */
}
/* ... early init failure ... */ goto REFUSE;  /* 0x7A2C CBZ W26 / 0x7A30 B */
REFUSE: print "if olock secure lock state, Not allow Fastboot.";
        ResetSystem(0x3E);                   /* 0x6CF0 */
        if (no_memcard_protocol) goto SPIN;  /* 0x6CF4 TBZ X19,#63 */
        goto LABEL_342;
SPIN:   print "No fastboot support for flashless chipsets, Infinte loop";
        for (;;) ;                           /* 0x6D08 */
```

`SPIN` is the worse of the two: no reset and no USB, so there is no key
window and no transport. `REFUSE` at least restarts, but an A/B-exhausted
device then resets forever.

v1 NOPed both resets, which turned the reset loop into `SPIN` and bricked a
device. v2 only kept the reset for the no-protocol case, leaving one route
into `SPIN`. v3 disabled `REFUSE` for the fastboot path only. **v4 removes
every route**, by rewriting the five branches that lead into the two bad
blocks:

| offset | stock | patched |
|---|---|---|
| `0x6CD8` | `CBZ W26, 0x6D64` | `B 0x6D68` |
| `0x6CF4` | `TBZ X19,#63,0x6D68` | `B 0x6D68` |
| `0x6D60` | `CBNZ W26, 0x6CDC` | `B 0x6D68` |
| `0x6D64` | `TBNZ X19,#63,0x6CF8` | `B 0x6D68` |
| `0x7A2C` | `CBZ W26, 0x6D64` | `B 0x6D68` |

plus `0x6D94` `BL <ResetSystem>` → `NOP` for the `fastboot_unlock_verify`
refusal.

`0x6CF0` and `0x6D08` keep their stock bytes; after these rewrites nothing
branches into `REFUSE` (`0x6CDC`–`0x6CF4`) or `SPIN` (`0x6CF8`–`0x6D08`), so
control always reaches `Launching fastboot`. Verified by control-flow
reachability over the whole function, not by inspection. The A/B retry
counter and slot switching are never touched.

Still present, deliberately: the **recovery** refusal block
(`0x6BC4`–`0x6CC8`, entered only when a recovery boot is requested *and* the
olock secure-lock state is set) ends in `ResetSystem(0x3E)` at `0x6CC8`, and
`0x7200` jumps to the same instruction from a misc-read failure. That path is
a state change plus a reboot, not the fastboot brick, and redirecting it
safely needs a live check of the register state at `0x6CCC`; it is left
untouched.

**patch13 — slot unbootable cleared, in RAM and on disk.** ABL keeps a
per-slot attribute word at `slot_info + 0x34` (priority 16..17, active 18,
retry 19..21, successful 22, unbootable 23). That word is the GPT entry
attribute field of `boot_a`/`boot_b` (bits 48..55), not part of `misc`.
patch13 makes bit 23 inert and erases it from the GPT, without touching retry
counts or slot switching:

- the setter `ORR W9, W9, #0x800000` becomes
  `AND W9, W9, #0xFF7FFFFF`. The very next instruction is
  `BL <write slot metadata>` (`0x20CDC`), so the cleared value lands in
  the GPT immediately;
- `0x20CDC` is the only function that writes slot metadata (to the primary and
  backup GPT)
  (`SetActiveSlot`, `ClearUnbootable` and `HandleActiveSlotUnbootable` all
  funnel through it). It byte-packs the attribute word per 0x80-byte GPT
  partition entry;
  the load producing the bits 16..23 byte, `LDRH W8, [X9, #6]`, becomes
  `AND W8, W8, #0x7F`. The halfword is already the upper half of the attribute
  dword, so its bits 0..7 carry priority/active/retry/successful/unbootable and
  only bit 7 is dropped. Every GPT attribute write — `set_active`,
  `ClearUnbootable`, the retry-count decrement, `MarkPtnActive` — therefore
  stores `unbootable = no` for every slot, whatever RAM or the previous image
  held. Nothing is cleared at boot time: a healthy boot performs no write;
- the shared bootable predicate leaf is forced to "successful", so
  `IsCurrentSlotBootable`, the `ClearUnbootable` precondition and the
  boot-image slot check see a bootable slot for the whole current boot,
  before the first GPT write lands;
- the decision read `UBFX X24, X27, #0x17, #1` and the counter read
  `UBFX W8, W8, #0x17, #1` become `MOV X24, XZR` / `MOV W8, WZR`.

Only bit 23 is touched: bits 16..22 (priority, active, retry, successful) are
preserved byte-for-byte, and the record layout/header handling stays exactly
as stock — one bit in one byte differs.

### patch11c — disabled-vbmeta cmdline

patch11 now has three parts: 11a forces the outbound device state to `locked`,
11b forces every verified-boot color consumer to `green`, and 11c keeps the
libavb handoff consistent with that spoof. When a top-level vbmeta has
`AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED`, stock libavb deliberately
omits `androidboot.vbmeta.{hash_alg,size,digest}` and relies on userspace
seeing `orange`. Once 11a/11b report `green`, fs_avb no longer permits that
omission and can fail first-stage mount.

11c NOPs the verification-disabled branch so the normal `avb_append_options`
path publishes the top-level vbmeta digest. The matcher keys off the stable
`LDR X8,[X23,#0x60]` instruction and accepts compiler-dependent `LDRB`/`TBNZ`
encodings; all tracked GBL ABLs, including Xiaomi 17 (pudding 44), are covered.

The old `--experimental-vbmeta-cmdline` flag is a deprecated no-op. Mode-2
packaging automatically passes `--require-patch11`: if 11a/11b/11c do not all
apply, packaging fails before producing an EFISP. This does not make a mixed
stock/custom vbmeta chain valid, and it still runs the managed-verity helper
that the disabled path normally bypasses, so test with `stage`/`oem boot-efi`
before writing EFISP.

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
