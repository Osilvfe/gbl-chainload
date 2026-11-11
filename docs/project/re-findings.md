# Reverse-engineering findings

This file preserves distilled facts only. Session transcripts are intentionally not kept as durable project docs.

## Mode-0 and mode-1 behavior

- Mode-1 fakelocks the `QCOM_VERIFIEDBOOT_PROTOCOL` view: reads report locked state, writes/reset are swallowed or normalized, and clean stock images can present locked/green state to ABL/KM.
- Mode-0 is the honest unlocked observation path plus universal preservation baseline; it is not identical to a clean stock baseline in every capture.
- Mode-1 handles stock recovery + custom system, but it does not by itself make arbitrary custom recovery metadata acceptable to userspace AVB.

Evidence to preserve:

- Mode-1 mutation was designed around verified-boot protocol state rather than blanket ABL state rewrites.
- The old “fakelocked vs debug” comparison showed that no-fakelock captures and stock captures should not be conflated.

## Reserve token / `oplusreserve1`

- The critical preserve target is the DeepTest / fastboot token block at `oplusreserve1` `LastBlock - 0x3a5`, 4 KiB LBA `1114`, offset `0x45a000`.
- User locked/unlocked diffs show that the lock path zeroes this block.
- `UpdateUnlockRecord` at `LastBlock - 0x35c` / LBA `1187` is accounting/state, not the primary fastboot authorization gate.
- `oplusreserve1` has other writers, so a blanket write-swallow is broader than token preservation. The safe policy is targeted token-block preservation plus narrow accounting/state handling when needed.

Evidence to preserve:

- Token preservation policy is grounded in a locked/unlocked raw image diff, not only static strings.
- The writer table included non-unlock writers such as charge, UART, and DDR-control paths; this is why blanket partition-wide swallowing is too broad for a final policy.
- Return-value handling for swallowed writes must keep callers on their expected success path.

## Fastboot authorization gate

- Fastboot authorization centers on `ReadSecurityState` bits plus token/OCDT/model paths.
- `FastbootUnlockVerify` is the single gate callsite identified in static analysis.
- UI-confirmed lock path zeroes the token block; direct/no-display `fastboot flashing lock` path did not show the same token-zero behavior in the reviewed evidence.
- Preserve strategy should prioritize token block preservation and, if needed, present a coherent locked-looking UnlockRecord; it should not treat UnlockRecord as the primary gate.

Evidence to preserve:

- The static close-out identified `FastbootUnlockVerify` as the gate callsite to remember.
- Security-state bits, OCDT/model data, and token state are coupled; changing only one surface may produce inconsistent behavior.

## Recovery normal-boot failure

- Custom recovery under mode-1 can complete recovery boot while failing normal Android boot.
- The failure is after ABL: userspace AVB / first-stage init re-reads the on-disk recovery metadata and rejects missing, invalid, or mismatched in-partition AVB footer/vbmeta.
- The selected fix is disk-side grafting of stock recovery vbmeta/footer bytes onto the custom recovery image, either host-side or device-side.

Evidence to preserve:

- Graft offset is `round_up(custom_image_size, 4 KiB)` for the custom recovery payload, then append or splice the stock recovery vbmeta/footer bytes so userspace AVB reads satisfiable metadata.
- AOSP references used during investigation: `first_stage_mount.cpp` `InitAvbHandle()`, `fs_avb.cpp` `AvbHandle::Open()`, `avb_ops.cpp` `FsManagerAvbOps`, and `avb_slot_verify.c` hash descriptor verification.
- `patch10` can force libavb success inside ABL, but that does not remove the later userspace AVB read of recovery metadata.

## Patch anchors and fixtures

- `patch6` lock-state fastboot gate anchors on refusal strings such as “Flashing is not allowed in Lock State”, “Erase is not allowed in Lock State”, “Slot Change is not allowed in Lock State”, and “Snapshot Cancel is not allowed in Lock State”. The rewrite is on the preceding branch gate, not on the strings themselves.
- `patch10` anchors on the unique libavb string `Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO`, walks back to the `avb_slot_verify` PACIASP function entry, forces allow-verification-error at entry, and forces OK at exit.
- `patch11` locked-handoff (方案1 "lifecycle split"): ABL stays internally unlocked, but every outbound handoff surface reports locked/green. GBL-era ABLs (OnePlus 15/15T, Xiaomi 17 verified) no longer carry an `androidboot.flash.locked` literal — userspace derives it from `androidboot.vbmeta.device_state`. The OS-facing state leaves ABL through exactly two producers, and patch11 covers both: **11a** — the unique `androidboot.vbmeta.device_state` string's ADRP+ADD xref is followed by `cmp wN,#0; csel x2, xLocked, xUnlocked, eq`, byte-identical `0x9A890142` across all five inspected builds (infiniti 703/201, fairlady 201/701-CN, pudding 44); rewrite to `mov x2, x10` (`0xAA0A03E2`) so the bootconfig value is always `locked`. **11b** — every `*verifiedbootstate` consumer (bootconfig entry, `UpdateCmdline`'s `androidboot.`/` oplusboot.` mirrors, debug prints) resolves the color through one `.data` table of 16-byte `{idx, ptr}` tuples (`{0:green, 1:orange, 2:yellow, 3:red}`); repoint the orange and yellow slots at the green string (red stays — it only occurs on verification failure, which patch10 forces to success). The `locked`/`unlocked` literals are shared with fastboot/menu display sites (e.g. `0x8708` cluster, `0x7e64`), so string-byte rewrites are unsafe and the patch anchors on the CSEL instead. patch11 is optional (`mandatory: false`) and idempotent (11a recognizes the rewritten `MOV xD, xLocked`; 11b accepts slots already equal to green). Cross-build gate `tests/host/097_patch11_multi_abl.sh`: applies + idempotent on all four tracked fixtures including non-oplus Xiaomi; frozen-byte parity on the EU 703 PE in `crates/patch-engine/tests/parity.rs`.
- `patch7` orange-state CBZ. The guard instruction is byte-identical across builds: `0x3400046A` = `CBZ W10, +0x8C` (imm19=35), rewritten to `0x14000023` (`B` same displacement). What moves between OTA builds is its surrounding context, so an EU-derived fixed byte anchor missed 201 (an extra `STR` shifts the CBZ from CSEL+4 to CSEL+8). `ApplyOrangeScreen` is **string-anchored**: scan for the warning text `"Your device has been unlocked and can't be trusted"` (invariant across builds, referenced by exactly one ADRP+ADD — note the bare `"Orange State"` has 2 refs and is ambiguous), resolve that pair with `Arm64FindAdrpAddTargeting`, then walk backward (≤0x40) to the nearest `CBZ Wn` and `RewriteBUncond` it (target preserved). The intervening instructions between CBZ and ADRP are X-form `CBZ/CBNZ`, stepped over by the W-form match; the guard is consistently ADRP-0x1C. Idempotency: after rewrite the guard slot is a forward unconditional `B`, which the backward scan recognizes as already-applied (PATCH_OK). No instruction-pattern fallback — the string anchor is strictly more robust, and a build divergent enough to break it would also break a byte pattern. Cross-build gate `tests/host/088_patch7_multi_abl.sh`: infiniti-EU-16.0.5.703 (CBZ @0x78F0), infiniti-IN-16.0.7.201 (@0x76D8), op15t-fairlady-201 (@0x76D8), fairlady-CN-16.0.7.200 (@0x76D8) all → OK + idempotent; non-oplus (xi17-pudding Xiaomi, myron) correctly MISS. Ghidra: bookmark+comment on the 201 CBZ @0x76D8 in the `gbl_root_canoe` project (`abl-201.efi`); the warning block it skips loads "Orange State" at +0x14 via ADRP@0x76EC.
- `patch1` efisp-recursion guard MISS on IN-16.0.7.201 is **expected, not a regression**. The guard only exists where the ABL contains the EFISP re-entry marker; `fv-unwrap` reports `efisp-marker: present` on EU-16.0.5.703 but `efisp-marker: absent` on IN-16.0.7.201, so there is nothing for patch1 to anchor on in 201. A staged `oem boot-efi` one-shot does not persist an ABL to EFISP, so the missing guard is harmless on the dev loop. (patch1's anchor is itself EU-specific; re-deriving it for other builds is a separate follow-up, tracked apart from patch7.)
- Fixture coverage lives under `tests/images/`; keep the fixture README as the live map for which raw FV wrappers or extracted PEs each patch test consumes.

## QSEE / KM / SPSS follow-ups

- QSEE/KM call-slot mapping and SPSS/Secretkeeper follow-ups were investigated enough to guide mode-2 planning, but mode-2 still needs profile lifecycle work before it becomes a usable mode.
- Preserve future notes as profile evidence: command IDs, payload shapes, and OTA/profile coupling should be stored here instead of in session transcripts.

## LogFs and logging

- Earlier LogFs mount failures came from staged EFI ordering and `ConnectController` assumptions.
- The durable fix pattern is probe-first `SimpleFileSystem`, tolerate `EFI_ALREADY_STARTED` / `EFI_NOT_FOUND`, and avoid remount churn.
- A second `LogFsInit` on the regular path is a likely avoidable cost center.

Evidence to preserve:

- Staged EFI and FV-loaded primary GBL contexts behave differently; mount code must tolerate both.
- Current minimal logging design removed the private `GblChainload_BootN.txt` mirror and relies on UefiLog rotation/snapshot behavior.

## Boot-time performance

- Largest suspected costs: ABL unwrap/LZMA, LogFs mount/rotation, then `LoadImage` / `StartImage`.
- Likely optimizations: skip duplicate LogFs init, read only wrapped FV bytes, and avoid double-decompression.
- Partition enumeration, FV driver loading, patch scans, and hook installation are second-order until measured otherwise.

## Live fixture note

- Test fixture naming and harness coverage remain documented in `tests/images/README.md`.

## GBL FastbootLib boot path (OnePlus 15T / fairlady, gbl-chainload-debug.efi)

- Path: `fastboot boot` handler `sub_2FF60` → `sub_32DE8` (load/authenticate) →
  `sub_3EA54` (AVB result + verified-boot menu) → `sub_D7DC` (`BootLinux`).
- `ORANGE` (state `[X19+0x438] == 1`) is the normal state of an **unlocked**
  device, not a signature failure. `sub_3F80C` reads `sub_A2B8()`
  (`IsUnlocked`) at `0x3F858`, stores it in the local at `0x3F8BC`, and at
  `0x40CEC` sets state `1` whenever it is non-zero; only when it is zero does
  the AVB result byte at `[X25]` pick GREEN (0) or YELLOW (2). The ABL boot path
  (`sub_3F80C`) computes it the same way, which is exactly why patch7 exists.
  Consequence: a stock-signed boot image still gets ORANGE on an unlocked
  device, and GBL has no patch7/patch10 of its own.
- The hang is the ORANGE menu (index 1: `<!>` + "The boot loader is
  unlocked..." + "PRESS POWER KEY TO PAUSE BOOT"), reached from
  `0x3F3AC B.EQ 0x3F428`. Menu entry 1 gets timeout `0xA` (10 s) at
  `[menu+0x4C]`, but the wait loop `sub_4C914` busy-waits via `sub_4FC4`
  while the key-detection timer event (`qword_AE0B0`) is still installed and
  never calls the event dispatcher. If that event's notify never runs, the
  event is never cleared and the loop never exits. This matches the observed
  behaviour: both a stock and a non-stock boot image stop on the same screen,
  so the blocker is the menu, not AVB. The transport stays alive; this is not
  a crash. The fix below does not depend on which of the two it is: skipping
  the menu removes the wait entirely.
- Fix (fastboot-only scope): at `0x3F428` rewrite
  `LDRB W8,[X29,#var_8]` (`0x394063A8`) to `B loc_3F4F8` (`0x14000034`).
  This skips the whole ORANGE menu block and resumes at the shared
  "continue boot" path. The block is only reachable from the state-1 branch
  (`0x3F3AC B.EQ`), and `BootLinux` is only reached from GBL `fastboot boot`,
  so normal chain-loaded Android boot is unaffected.
- dtbo overlay in `BootLinux`: every overlay application funnels through
  `sub_10FB8` → `sub_10AB4` → `sub_2681C` → `sub_25FC0`
  (`ufdt_overlay_apply`). At `0x10B24` (`BL sub_2681C`, `0x9400573E`) replace
  with `MOV X0,X21` (`0xAA1503E0`); `X21` is `sub_25F00`'s main-FDT copy and
  is non-NULL there, so the call reports success without modifying the DTB and
  the later size/copy path still runs.
  Do **not** return `0`: `sub_2681C` returns a pointer on success.
- `androidboot.dtbo_idx` is emitted by `sub_1BD50` at `0x1CA1C`
  (`B.EQ loc_1CA6C`, `0x54000280`). Rewrite to `B loc_1CA6C`
  (`0x14000014`) to suppress it. `androidboot.dtb_idx` still describes the
  selected base DTB and is kept.
- The Fastboot screen's `BUILD - <name>` line lives at `0x77458` (19-char
  name). `--mark` rewrites it to `gbl-chainload-dbgfx` / `gbl-chainload-nodtb`
  so the live payload is identifiable on screen; the length is unchanged.
- `sub_4C914` is the shared menu wait loop:
  `while (qword_AE0B0 != 0) sub_4FC4(10000);`. `qword_AE0B0` is the
  key-detection timer event created by `sub_4C9B8`; only its own notify
  (`sub_4CABC` → `sub_4C854`) clears it. `sub_4FC4` is a pure `CNTPCT_EL0`
  delay and does not pump events, and in the GBL `fastboot boot` context the
  loop was observed not to recover. Treating that as a hang is an observation,
  not a proof: timer-event notify runs from the timer interrupt. Either way,
  `--no-menu-wait` rewrites the prologue to `MOV W0,#1; RET`
  (`0x52800020`, `0xD65F03C0`) and removes the dependency on it.
- Tooling: `scripts/gbl-fastboot-boot-fix.py --boot-menu [--no-dtbo] [--no-menu-wait] [--mark X]`
  validates the original instruction words, is idempotent, and refuses any
  other base EFI. Offsets are file offsets because the PE has ImageBase 0 and
  file offset == RVA. Base EFI used here:
  `gbl-chainload-debug.efi` sha256
  `5f849600c0fa2912ce4622c924240917080b58465588c4c3b0d2b298e11b38a8`.

### Why `fastboot boot` still stops after the menu fix (DTB match)

With the ORANGE menu and `dtbo_idx` edits applied, `fastboot boot` reaches
`BootLinux` and then goes silent. The GBL log (`fastboot boot`, slot `_b`)
shows:

```
[57201]Memory Base Address: 0x80000000
[57307]No match found for Soc Dtb type
[57307]Error: Appended Soc Device Tree blob not found
```

The 106 ms gap is the DTB scan, not a hang: `BootLinux` calls
`sub_12648(dtb_base, dtb_end, dtb_start)` (`0xEBDC`), which walks the FDTs in
the `vendor_boot` v4 DTB area and returns 0 when nothing matched, so
`0xEBE0 CBZ X0, 0xED78` takes the error path and `BootLinux` returns without
sending a fastboot FAIL.

Layout check on this device's `vendor_boot_b` (`/tmp/vb_b.img`): page 4096,
header 0x850, `vendor_ramdisk_size` 0x12ddc5d, so the DTB area starts at
`0x1000 + align_up(0x12ddc5d, 0x1000) = 0x12df000`; `dtb_size` is 0x744972 and
the area holds 14 back-to-back FDTs. All 14 carry only `qcom,msm-id` (two
cells) and `qcom,board-id = <0 0>`; `qcom,msm-id` covers 0x294/0x295/0x2ad/
0x2d7/0x2c0 plus 0x1000xxx variants, with soc-rev 0x10000/0x20000.

The match gate is inside `sub_128EC`:
`LDRH [dtb_msm_id]` vs `(u16)sub_D344()`; only a hit sets bit 58
(`0x0400000000000000`), and at `LABEL_302` (`*v8 >> 0x3a & 1`) nothing else is
accepted. `sub_D344` returns `dword_9726C`, `sub_D350` returns `dword_97280`
(the `qcom,msm-id` second cell). Both live in `.data`, are **zero in the file,
and have no writer anywhere in the GBL image** — xrefs to the whole
`0x97240..0x972A8` block only ever read them, and no literal-pool reference
exists. The ABL populates its copies through the DevInfo protocol
(`sub_4E50` in the stock ABL); GBL's boot path never does.

Ground truth for this board comes from the stock ABL's own log (UEFILog1.txt):

```
soc PfId= 0x294 Soc= 0x20000 Fid= 0x0 DVId= 0x8 ...
board PfId= 294 Soc= 20000 ...
```

so `BoardSocId = 0x294` and `BoardSocRev = 0x20000`, which selects the
"Canoe v2 SoC" family (0x294 / 0x20000) out of the 14 blobs.

Fix (`--soc-info`, scoped to GBL): rewrite `sub_D344` / `sub_D350` to return
those constants — `MOV W0,#0x294` / `MOV W0,#0x20000`, the following `LDR`
becomes `NOP`, `RET` stays. This also fixes the `hw-revision` getvar and the
running-DTB node check at `0x11F74`, which read the same two getters.

### Second gate: `qcom,board-id` on the dtbo entries

With id/rev patched, `fastboot boot` advances past `sub_12648` and dies one
layer later, in `sub_143A0` (dtbo-table search, called from `0xEC04` when
`sub_113D0()` reports an exact vendor_boot match):

```
[47051]Memory Base Address: 0x80000000
[47158]Unable to find the Board Dtb
[47158]Error: Board Dtbo blob not found
```

`dtbo_b` is a standard DT table (magic `0xd7b7ab1e`, 8 entries, 32-byte
entries, `entries_offset=0x20`). Every entry's root carries
`qcom,msm-id=<...>` and `qcom,board-id = <8 0>`, plus `oplus,project-id`
(0x64DD or 0x650F) and `oplus,hw-id`. `sub_143A0` runs the same `sub_128EC`
matcher but with `a3 = 57`, i.e. it accepts a candidate only when **bit 57**
is set, and bit 57 is the variant gate:

```
LDR  W8,[X20,#0x14]     ; board-id cell 0, byte-swapped
AND  W21,W8,#0xFF       ; 0x08000000 -> 0x08
BL   sub_C9D4           ; dword_97258: BoardVariant / DevInfo DVId
CMP  W21,W0
B.NE fail
ORR  X8,X8,#0x200000000000000
```

`sub_C9D4` reads `dword_97258`, which is **also never written in GBL** and is 0
in the file, while the dtbo entries and the ABL log (`DVId= 8`) both say 8.
Result: no dtbo entry can ever satisfy bit 57, so `v32` stays 0 and the board
DTB lookup fails. (`sub_D398`, which mixes `dword_97258/5C/60`, depends on the
same value.)

Fix: rewrite `sub_C9D4` (`0xC9D4`) to `MOV W0,#8` (`0x52800100`), `NOP` the
`LDR`, keep the `RET`. `--soc-info` now applies all three getters
(`--soc-id`, `--soc-rev`, `--board-variant` override them).

### Verified: GBL `fastboot boot` works once the DTB matcher is fed real data

With `--soc-info` applied, `fastboot boot` from GBL's own FastbootLib boots
UniXNU/Mu-Silicium (`UniXNU-fairlady-run220-turnip-metal-boot.img`, a v4 boot
image: 4096-byte page, gzip `Image` at `0x1000`, `ramdisk_size = 0`, no
appended DTB, empty cmdline). Sequence in the GBL log:

```
Memory Base Address: 0x80000000
Apply Overlay total time: 120 ms
Update Device Tree total time: 74 ms
Shutting Down UEFI Boot Services: ...
Start EBS
```

So `sub_12648` → `sub_143A0` → `sub_10FB8`/`sub_10AB4` → `sub_15FCC` →
EBS → kernel handoff is functional. The earlier "hang" was entirely the two
uninitialised platform-info getters, not the boot path.

### Stock Android still cannot boot from GBL's FastbootLib

A/B comparison of the *same* `fastboot boot /home/osilvfe/Downloads/boot.img`
(96 MiB stock boot image, `ramdisk_size = 0`, ramdisk lives in vendor_boot):

| step | ABL fastboot (boot 320, succeeds) | GBL FastbootLib (boot 318, silent) |
|---|---|---|
| `soc PfId= 0x294 Soc= 0x20000` | yes | absent |
| `Booting from slot (_b)` | yes | absent |
| `Loaded Partition: pvmfw`, per-slot AVB | yes | only the downloaded image |
| `AddOppoBootModeBootconfigLen` | 1246 | absent |
| `AddOppoBootModeCmdLineLen` | 2469 | absent |
| cmdline | `androidboot.*`, `oplusboot.*`, `rootwait ro init=/init`, ... | vendor_boot's `video=vfb ... console=ttynull buildvariant=user msm_drm...` |
| `Modifying Host Kernel DT to add AVF data` | yes | absent |

The 2469-byte cmdline / 1246-byte bootconfig are produced inside ABL from
`oplusreserve1`, RPMB boot info, project name, DDR info, SKU queries and
`system_debug` data. GBL has none of that logic. Without `root=` and
`androidboot.slot_suffix`, and with `boot.img` carrying no ramdisk (the real
one is in `vendor_boot`), the kernel never reaches a point where it can panic,
which is why `pstore` stays empty and `/data/debugging/last_kmsg.txt` is never
created. Boot reason afterwards is `0x0 / Normal Boot`, not a kernel panic.

Conclusion: GBL's FastbootLib is usable for self-contained images (UniXNU-style
payloads that carry their own cmdline semantics and do not need the oplus
parameters). Booting stock Android through it would require porting ABL's
`AddOppoBootModeCmdLine`/`AddOppoBootModeBootconfig` generation and the
vendor_boot ramdisk handoff into GBL.

### Why the load-address prints never appear (`--log-all`)

`sub_3E2C(level, fmt, ...)` → `sub_3E80` → `sub_3EB4`, whose first statement is
`if ((sub_38274() & level) == 0) return;`. `sub_38274()` returns
`dword_54AF8`, which is `0x80000042` here: only `ERROR` (0x80000000), `0x40`
and `0x2`. The `BootLinux` prints (`Kernel Load Address`, `Ramdisk Size
Actual`, `Device Tree Load Address`, ...) use level `0x400000`, so they are
filtered inside the logger — not by any branch. The surrounding
`TST W0,#0xFF; B.EQ` sequences are dead code because `sub_4348(x)` is just
`x != 0`, and every call passes a non-zero level. Patching those branches
changes nothing; `--log-all` instead rewrites `sub_38274` (`0x38274`) to
`MOVN W0,#0` (`0x12800000`), NOPs the `LDR`, and keeps the `RET`.

### `--no-dtbo` sites are on the live path

`ApplyOverlay` is `sub_10FB8` → `sub_10AB4`, called from `BootLinux` at
`0xEE48` and `0xEF48` — after DTB selection, on the same path the UniXNU boot
takes. The overlay apply is `0x10B24 BL sub_2681C`; rewriting it to
`MOV X0,X21` returns the main FDT from `sub_25F00` untouched. `UpdateCmdline`
(`sub_1BD50`, called once at `0xF0BC`) emits ` androidboot.dtbo_idx=`; its
`0x1CA1C B.EQ` is rewritten to `B` to suppress the property.

### `oem boot-nodtbo` on a v2 boot image: `&BootForceNormalBoot` aborted the boot

`fastboot oem boot-nodtbo` (source-built GBL) reached `BootLinux`, selected the
DTB appended to the boot image, and then reset the device before
`ExitBootServices`. A direct-to-logfs trace (`\GblTrace.txt`) narrowed it to
`UpdateCmdLineParams()`:

```
cmdline: params in cmd=0x3F7 cfg=0x0
cl-step: 11
cl12: rec=0 ms=1
cl-step: 12 / 13 / 14        <- then the device reset
```

`UpdateCmdLine.c` did:

```c
CHAR8 BootForceNormalBoot = '0';          /* a single CHAR8, no NUL */
...
Src = &BootForceNormalBoot;
AsciiStrCatS (Dst, MaxCmdLineLen, Src);
```

`AsciiStrCatS` runs `AsciiStrnLenS (Source, CopyLen)` first, so it scans past
the 1-byte object looking for a terminator. The other call site is safe
because it passes an explicit length (`sizeof (BootForceNormalBoot)`).

Fix: append a NUL-terminated 2-byte buffer holding the current value. The
bootconfig path is unchanged.

Why this only showed up now: the block is gated on
`HeaderVersion <= BOOT_HEADER_VERSION_THREE`. UniXNU/Mu-Silicium images are v4
and skip the entire block; the mainline image is v2, so v2/v3-only code ran for
the first time on this device.

With the fix, the trace completes:

```
cl-step: 15 / 16 / 17 / 18
cmdline: params=Success
cmdline: bootcfg=Success
cmdline: cleared
cmdline: status=Success
dt-update: status=Success
pre-ebs
```

Trace tooling: `GblChainloadPkg/Library/LogFsLib/Trace.c`. Every line is
`<uptime_ms> <text>` and is flushed immediately, because `UefiLog1.txt` only
reaches logfs at "write final log" just before `ExitBootServices` — a boot that
dies earlier leaves no platform log at all. The file is truncated by
`LogFsInit()` and capped at 48 KiB; the handle is kept open between lines.
The call sites are compiled out unless `GBL_TRACE` is defined, which only
`GblChainloadPkg.dsc` does, so an ABL build of `QcomModulePkg.dsc` is
unaffected.

### `LibUfdt` overlay arena: stale bump offset and a reachable double free

`EmbeddedPkg/Library/LibUfdt/sysdeps/libufdt_sysdeps_vendor.c` backs
`ufdt_install_blob` / `ufdt_apply_multi_overlay` with a single 5 MiB bump
allocator:

```c
static void* buffer;
void *dto_malloc(size_t size) { static int ssize = 0; ... }
```

Three defects, all in that ~20-line block:

1. `ssize` was a function-local `static` that nothing ever reset. A second
   overlay in the same boot started allocating from the first overlay's end
   offset, so `dto_malloc` could fail (return `NULL`) even on a fresh buffer.
2. `post_overlay_free()` freed `buffer` but left the pointer non-`NULL`. That
   is reachable as a **double free**: `ApplyOverlay()` does
   `if (DtsList == NULL) goto out;` before `pre_overlay_malloc()`, while `out:`
   unconditionally calls `post_overlay_free()`. A second call with no overlays
   therefore frees an already-freed pointer.
3. `pre_overlay_malloc()` overwrote `buffer` without freeing the previous
   allocation, so the two early `return EFI_NOT_FOUND` paths in `ApplyOverlay`
   (install-blob failure, apply-overlay failure) leaked 5 MiB per attempt.

Fix: move the offset to a file-scope `buffer_used`, reset it in both entry
points, free-and-clear in `pre_overlay_malloc`, and clear in
`post_overlay_free`. `dto_malloc` now also returns `NULL` when no arena is
active instead of doing pointer arithmetic on `NULL`.

The allocator's usage window is what makes resetting safe: pointers handed out
by `dto_malloc` only live between `pre_overlay_malloc()` and
`post_overlay_free()`, and `ApplyOverlay` copies the final FDT to
`DeviceTreeLoadAddr` before freeing the arena.

Not the cause of the no-dtbo reset (that was the `&BootForceNormalBoot`
overread); `fastboot oem boot-nodtbo` skips the overlay entirely. It matters
for the normal `fastboot boot` path, which can apply the board DTBO, hyp
DTBOs and the AVF DP DTBO through the same arena in one boot.
