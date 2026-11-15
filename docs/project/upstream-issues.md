# Upstream issue triage

Snapshot date: **2026-11-15**. Source repositories:

- `1vivy/gbl-chainload`
- `1vivy/edk2-gbl-chainload`

Upstream is no longer actively maintained. This file records the open items we
reviewed, their actual failure scenarios, and the intended disposition. Re-check
the live issues before acting; this is a snapshot, not a mirror.

## Executive summary

- No current open issue is P0/P1 for the supported GBL-only test loop
  (`fastboot stage` + `oem boot-efi`).
- `#42` (BBG) and `#43` (ZIP logging) are installer-ZIP issues. While
  persistent writes go through GBL/EFI instead of a booted-Android kernel, both
  are downgraded to P3.
- The useful near-term work is edk2 `#14` (hide the menu AVB warning under
  profile spoof) and porting PR `#21` (detect duplicate protocol hooks).
- edk2 `#10` and `#13` need explicit product/safety decisions before code.

## edk2 issues

### #14 — hide AVB warning under mode-2/profile

Location: `edk2/QcomModulePkg/Library/BootLib/FastbootMenu.c` (AVB warning draw
block) and `GblVbmetaBuildWarning()` in
`edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c`.

Behavior today: the fastboot menu computes and draws
`AVB WARNING - verify-fail: ... - graft required` unconditionally. Under a
successfully loaded mode-2 profile this is misleading: profile spoof owns the
userspace AVB view and a graft is not the intended remedy.

Proposed fix: expose a runtime "profile spoof active" flag from BootFlow to the
FastbootLib menu path. Suppress only the menu warning when profile spoof loaded
successfully; keep `vbmeta:warning` getvar available for diagnostics, and keep
the warning when the profile is missing or invalid.

Priority: P3, low risk, code-actionable.

### #13 — remove `abl` from critical partitions list

Location: `CriticalPartitions[]` in
`edk2/QcomModulePkg/Library/FastbootLib/FastbootCmds.c` (`L"abl"` is the first
entry). `CmdFlash` also rejects writes when `IsUnlocked()` is false before it
checks the critical list.

The current list is a safety guard, not a bug. Removing `L"abl"` alone is not
guaranteed to make `fastboot flash abl` work under mode-1/2, because fakelock
presentation can make `IsUnlocked()` read false. Options:

1. Remove `L"abl"` directly — simplest, but exposes ABL overwrite to accidental
   or wrong-image writes; a bad ABL can hard-brick the chainload path.
2. Add an explicit `oem flash-abl <commit>` command with a confirmation word and
   an already-unlocked requirement.
3. Gate removal behind a build-time opt-in such as `GBL_ALLOW_ABL_FLASH`.

Recommendation: option 2. Do not remove the guard just to make the issue title
true.

Priority: P2, needs owner decision.

### #10 — OEM unlock allowed does not persist

Location: `ReadAllowUnlockValue()` and the startup read in
`FastbootCmds.c`; the FRP byte is the LSB of the last byte of the `frp`
partition.

Root cause confirmed upstream: the GBL write persists, but AOSP's
`OemLockService` / `PersistentDataBlockService` mirrors the Developer Options
"OEM unlocking" state back to the same FRP byte on every system boot. With the
toggle off in Android, the next GBL fastboot session reads `0` again.

Candidates:

1. Read-side lie: return `1` from `ReadAllowUnlockValue()` under fakelock/profile
   spoof without touching the disk.
2. Seed `IsAllowUnlock = 1` at FastbootLib init and skip the FRP read.
3. Treat as by-design; require toggle + unlock in the same fastboot session.

Blocker for 1/2: the edk2 FastbootLib runs in the cached ABL image and cannot
see GBL's `gManifest`. A bridge (configuration table carrying the capability
bits, or the pre-hook true unlock state) is needed first. Otherwise the change
would unconditionally weaken FRP enforcement on locked devices.

Recommendation: read-side allowance only for a device already truly unlocked;
keep FRP enforcement for locked devices. Otherwise document option 3.

Priority: P2, needs design decision.

## gbl-chainload issues

### #42 — BBG affects ZIP flashing

BBG here is **Baseband Guard**, a Linux LSM shipped by some custom kernels. Its
default allowlist covers `boot`, `init_boot`, `vendor_boot`, `dtbo`, `userdata`,
`cache`, `metadata`, `misc`, `vbmeta*`, and `recovery`; it does **not** include
`efisp` or `abl`. Writes to non-allowlisted block devices are denied at the
kernel write path.

Scenario: an installer ZIP is run from booted Android (the `BOOTMODE` path) on
a BBG kernel. `gbl commit` writing `efisp` or `abl_<slot>` gets `EPERM`, while
the same ZIP in recovery (kernel without BBG) succeeds. Depending on which
write is denied and whether the failure is propagated, the ZIP may abort at
EFISP, abort only after EFISP was already written, or appear to succeed without
persisting.

Why this is not P0 for us: GBL/ABL fastboot and EFI `BlockIo` execute before
Linux, so BBG cannot intercept them. The issue only applies to direct block
writes from a booted-Android kernel (`dd` or `gbl commit` inside the ZIP).

ZIP-side mitigations when that line of work resumes: detect
`baseband_guard` in `/sys/kernel/security/lsm` (or `CONFIG_BBG=y`) and abort
before writing; require recovery flashing; make every write read back and
verify; log the failing step to logfs.

Priority: P3 while the supported write path is GBL/EFI.

### #43 — installer ZIPs should log to logfs

ZIP install order is EFISP first, then loader-ABL restore
(`modes/install-common.sh`). After an OTA such as 703 -> 201, the inactive slot
ABL is no longer vulnerable, so the ZIP must restore a vulnerable ABL from the
active slot or `backup_abl.img`. If that step fails silently, EFISP has already
changed while the target ABL still lacks the EFISP loader path; the next boot
looks like a failed install.

Default logs and backups live under `/sdcard/gbl-chainload`, i.e. `/data`. A
first-time blank-EFISP install can change verified-boot state and trigger a
`/data` format; if Android no longer boots, `/data` logs are unavailable. That
is exactly the case where the failure needs post-mortem evidence.

Logfs is the correct destination: GBL already writes `GblTrace.txt` there, and
it remains readable when `/data` is gone. The ZIP should mirror each step,
target slot, source image, and readback hash into a dedicated logfs file.

Priority: P3 while the ZIP path is not shipped; required before ZIP delivery.

### #50 — vbmeta / true-lock question

This is a design boundary, not a bug. A genuinely locked bootloader enforces
AVB; arbitrary unsigned custom partitions cannot both verify and be arbitrary.
The practical options are: unlock (state becomes visibly unlocked), use a
permissive/spoofed verification path (what patch11 does outbound), or re-sign
with a trusted custom key/fuse (not a practical distribution path).

Patch11's locked-handoff is deliberately the second option: ABL remains
internally unlocked while outbound state reads `locked`/`green`. It is a
presentation layer, not a true relock.

Priority: documentation only.

## Unmerged pull requests

- `#21 detect-only hook registry` — useful idea: fail closed when the current
  UEFI session already has GBL protocol hooks installed, instead of wrapping
  them again on a second `oem boot-efi`. The patch predates the `GBL_MODE`
  collapse and must be ported to `gManifest`. Port candidate.
- `#22 cache-ABL safety and docs consolidation` — mostly superseded by the
  appended GBLP1 overlay / on-device insertion model. Do not merge wholesale;
  salvage only checks that are still absent (for example the efisp recursion
  invariant, if not already covered).
- `#51 macan/sm8845` — already merged into our `main` as `d4b3923`; the upstream
  PR remaining open does not affect us.

## Next actions

1. Implement edk2 `#14` behind the runtime profile-spoof state.
2. Port PR `#21` to the manifest-driven hook registry.
3. Decide `#13` (prefer explicit `oem flash-abl`) and `#10` (prefer true-unlock
   read-side allowance).
4. Add the `#50` boundary text to user-facing documentation.
5. Revisit `#42`/`#43` only when installer-ZIP delivery resumes.
