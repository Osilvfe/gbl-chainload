# Engine Rework — Capability-Driven Engine, Single EFI

**Date:** 2026-05-22
**Status:** Design (pre-implementation)

## Problem

The on-device engine collapses three orthogonal axes into one integer (`GBL_MODE`):

1. **Which binary patches** land in the cached ABL (`DynamicPatchLib` groups
   `universal`, `mode_1`, `oem/oneplus_canoe`).
2. **Which protocol hooks** are mandatory vs. observe-only at boot
   (`ProtocolHookLib` — `VerifiedBoot`, `Qseecom`, `Spss`, `Scm`, `BlockIo`).
3. **Whether a mode-2 spoof profile** is loaded and applied.

This collapse has three concrete costs:

- **Group naming leaks the mode taxonomy into a generic patch engine.**
  `mode_1` is really *libavb-bypass + fakelock-fastboot-gates*, a capability,
  not a mode. `oem/oneplus_canoe` is purely cosmetic (orange-state warning
  skip) but is artificially gated to mode-2 only.
- **Three compile-time EFIs.** `GBL_MODE` is a `#define` driving `#if (GBL_MODE
  == N)` gates in every hook file. Each mode is a separate build artifact;
  composing capabilities outside the three blessed presets requires editing C
  and rebuilding.
- **`patch1`** zeroes the UTF-16 "efisp" string in the cached ABL to stop
  stock ABL from re-loading us out of EFISP after we LoadImage the patched
  ABL. The same goal is reachable from `BlockIoHook` (return `EFI_NO_MEDIA`
  on EFISP `ReadBlocks`), making the binary patch unnecessary and the
  post-patch invariant scan in `PatchEngine.c` redundant.

## Goals

- Replace `GBL_MODE` (integer) with capability flags throughout the engine.
- Collapse the three pre-built EFIs to **one** EFI binary, with capability
  selection driven by a small **runtime manifest** carried in the GBLP1
  overlay (alongside `cached_abl` and `mode2_profile`).
- Retire `patch1` and the post-patch `efisp` invariant scan; replace with an
  unconditional `BlockIoHook` EFISP read-fail.
- Rename `mode_1/` → `libavb_bypass/`; document `oem/` as capability-keyed
  and expandable; decouple `--oem` from mode in host tools.
- Preserve the user-facing **"mode 0/1/2" vocabulary** as host-side
  *presets* that expand to capability sets — no change in install UX for
  existing users.
- Move `detect_oem` from `mode-2-install.sh` into `install-common.sh` as
  shared infrastructure (one caller today; a stepping stone for more).

## Non-goals

- Renaming `--no-mode1` polarity in `abl-patcher` (deferred; see *Deferred
  follow-ups*).
- Adding new OEM patch groups beyond OnePlus/Canoe (groundwork only).
- Runtime composition of DynamicPatchLib patches. Patches stay chosen at
  **host packing time** and land baked into the cached ABL; only **hooks**
  become manifest-driven.
- Migrating `gbl-commit`, `vbmeta-graft`, `mode2-profile`, `gblp1-inspect`
  beyond the minimum needed to emit/inspect the new manifest entry.

## Design

### 1. Capability vocabulary

The engine speaks four capability flags. These are the only on-device
knobs; everything else is derived from them.

| Capability | Meaning | Drives |
|---|---|---|
| `GBL_LIBAVB_BYPASS` | Cached ABL has the libavb-force-success + lock-state fastboot-gate rewrites applied | Host patcher invocation; advisory marker in manifest |
| `GBL_FAKELOCK_HOOK` | Install the VerifiedBoot fakelock mutation path (clear `is_unlocked`/`is_unlock_critical`, swallow `WRITE_CONFIG`) | `InstallAll.c` VerifiedBoot Required gate + `Mode1Overlay` mutation gating |
| `GBL_OEM` (enum: `NONE`, `ONEPLUS`) | Host-time OEM patch group selection; today drives orange-state-skip (`patch7`) | Host patcher invocation; advisory marker in manifest |
| `GBL_MODE2_SPOOF` | Apply the mode-2 KM/RoT/SPSS rewrites against the GBLP1-packed profile | `InstallAll.c` SPSS/Qseecom Required gates; `Mode2Rewrite` mutation gating |

A capability flag is either set (1) or clear (0). Where today's code says
`#if (GBL_MODE == 1)`, the rework says `if (gManifest.WantFakelockHook)`.
Where today's code says `#if (GBL_MODE == 2)`, the rework says
`if (gManifest.WantMode2Spoof)`. The five mode-specific hook gates collapse
to four capability gates (no integer left).

`GBL_LIBAVB_BYPASS` and `GBL_OEM` are *advisory* on-device — they describe
what the cached ABL already had patched at host time. The on-device engine
does not act on them directly; they live in the manifest for diagnostics
(`gblp1-inspect`) and to let `gbl-fastboot` surface "what is this
overlay?" answers.

### 2. The manifest

A new GBLP1 entry type `GBLP1_TYPE_MANIFEST = 0x0020` (fresh non-zero
`uint16_t`, distinct from `0x0010 mode2_profile` and the `cached_abl` type).
Payload schema (little-endian, fixed 16 bytes; reserved/padding zeroed):

```
+0  uint32_t  magic              = 'GMAN'
+4  uint16_t  schema_version     = 1
+6  uint16_t  capability_bits    (see below)
+8  uint8_t   oem                (0=NONE, 1=ONEPLUS)
+9  uint8_t   reserved_pad[7]    = 0
```

`capability_bits`:

```
bit 0   want_libavb_bypass    (advisory; documents host-patch state)
bit 1   want_fakelock_hook    (active; gates VerifiedBoot+Mode1Overlay)
bit 2   want_mode2_spoof      (active; gates SPSS+Qseecom mode-2 rewrites)
bits 3-15  reserved (must be 0)
```

Validation rules:
- Unknown bits set → reject overlay (`GBL_PAYLOAD_BAD_MANIFEST`).
- `schema_version != 1` → reject.
- `reserved_pad != 0` → reject.
- `oem ∉ {0,1}` → reject.

The existing per-entry SHA-256, offset, and size checks in
`PayloadParse.c` apply unchanged.

### 3. Manifest forward-compat / absence

The parser already silently skips unknown entry types
(`PayloadParse.c:51-56`). So:

- **Old EFI loads new GBLP1**: the manifest entry passes integrity checks
  and is skipped by the old `find_entry()` callers. Old binaries keep
  working with new overlays.
- **New EFI loads old GBLP1** (no manifest entry): `find_entry(MANIFEST)`
  returns `OK` with `*out == NULL`. New EFI maps that to **all capability
  bits clear** — effective mode-0 / pure observation. **This is the safe
  default.**
- **New EFI loads new GBLP1**: manifest parsed and stashed via a new
  `GblPayload_LoadManifest()` library function paralleling
  `GblPayload_LoadMode2Profile()`.

### 4. Runtime: single EFI, manifest-gated hooks

`BootFlow.c` order (changes marked **NEW** / **CHANGED**):

```
1. EnumeratePartitions, LogFsInit  (unchanged)
2. GblPayload_LoadCachedAbl OR RunDynamicPatchOnSlotAbl  (unchanged)
3. GblPayload_LoadManifest (NEW)
     → on success: stash gManifest
     → on absent / parse failure: gManifest = all-zero (effective mode-0)
4. if (gManifest.WantMode2Spoof) {                              (CHANGED)
     GblPayload_LoadMode2Profile; Mode2_SetProfile;
     if missing: print "MODE-2 PROFILE MISSING — booting honest...";
   }
5. ProtocolHook_InstallAll (&HookRes)  (CHANGED — see §5)
6. LogFsClose; LoadImage(patched ABL); StartImage  (unchanged)
```

The mode-2-profile-missing console message + fastboot warning behavior is
preserved — what changes is the *gate* (was `#if (GBL_MODE == 2)`, now
`if (gManifest.WantMode2Spoof)`).

### 5. `ProtocolHook_InstallAll` — manifest-driven gates

The five `#if (GBL_MODE == N)` install-result gates in `InstallAll.c`
become runtime checks. The required/optional matrix is preserved exactly:

| Hook | Required when... | Today (`GBL_MODE`) | Rework (`gManifest`) |
|---|---|---|---|
| BlockIo | Always | always | always |
| Scm | Always | always | always |
| VerifiedBoot | Fakelock hook wanted | `GBL_MODE == 1` | `gManifest.WantFakelockHook` |
| Qseecom | Fakelock or spoof wanted | `GBL_MODE == 1 \|\| GBL_MODE == 2` | `gManifest.WantFakelockHook \|\| gManifest.WantMode2Spoof` |
| Spss | Spoof wanted | `GBL_MODE == 2` | `gManifest.WantMode2Spoof` |

`Mode1Overlay.c` and `Mode2Rewrite.c` mutation paths (currently
`#if`-wrapped) become unconditional functions whose call sites in the hook
wrappers check the manifest:

```c
// VerifiedBootHook.c — VBRwDeviceState slot
if (gManifest.WantFakelockHook && cfg == READ_CONFIG) {
  Mode1Overlay_ClearUnlockBits(out);
}
// SpssHook.c — SPSSDxe_ShareKeyMintInfo slot
if (gManifest.WantMode2Spoof) {
  Mode2Policy_RewriteSpss(info, GBL_SPSS_INFO_LEN);
}
```

Cost: one cached load + one predicted branch per intercepted call. Below
measurement threshold. Audit story weakens marginally (every binary
contains every mutation path) but the manifest is user-supplied via their
own host tool, not attacker-controlled — acceptable trade for one EFI.

### 6. `BlockIoHook` EFISP gate — retire `patch1`

`BlockIoHookRecord` gains an `IsEfisp` flag, matched at install time by
the GPT partition name (`"efisp"`, case-insensitive, matching the rule
`AblUnwrapLib` and the existing `oplusreserve1` detection use).

`HookedReadBlocks` and `HookedWriteBlocks` short-circuit on EFISP records:

```c
if (Rec->IsEfisp) {
  // Stop stock-ABL recursive load of gbl-chainload from EFISP.
  // Both reads and writes refused; second-stage ABL treats the
  // partition as unreadable and skips the GBL loader probe.
  GBL_INFO ("BlockIo: refused %a on EFISP (%a LBA=0x%lx)\n",
            IsRead ? "read" : "write", Rec->Name, Lba);
  return EFI_NO_MEDIA;
}
```

Choice rationale: `EFI_NO_MEDIA` matches "partition exists but has no
readable content," which is exactly the semantics we want to project to
the second-stage ABL. Alternatives considered:

- **`EFI_DEVICE_ERROR`** — viable but ambiguous; "device error" suggests
  retryable transient.
- **Hide the handle entirely** (uninstall `EFI_BLOCK_IO_PROTOCOL` on the
  EFISP handle) — more invasive, fights EDK2's handle lifecycle, harder
  to reason about. Skip.

The hook is **unconditional** (installs in every mode, fires on every
call). Required-status in `InstallAll.c` stays "always required, abort on
install failure." Recursion guarantee: install order is hooks-before-
`LoadImage(patched ABL)`, confirmed in `BootFlow.c:187-206`; the
second-stage ABL's first EFISP probe hits our wrapper.

Recovery surface preserved: `Entry.c:190` performs the 3-second VolUp key
wait **before** `BootFlowChainLoad()` runs, so the hook is not yet
installed during the key wait. A user holding VolUp reaches fastboot
without the EFISP gate active, and can `fastboot flash efisp <known-good>`
or `fastboot stage <test.efi> + oem boot-efi` to recover.

Consequent deletions:
- `DynamicPatchLib/universal/universal.c` patch table loses the `patch1-efisp-recursion` entry. The `universal` group becomes empty; either the directory is removed or it's kept as a placeholder for future universal patches with `kUniversalPatchesCount = 0`. **Decision:** keep the directory and the `SCOPE_UNIVERSAL` enum value (groundwork for future patches), make the patch table empty.
- `DynamicPatchLib/Internal/PatchEngine.c:101-142` post-patch efisp invariant scan: removed. Patches succeed/fail purely on their own outcomes.
- `tools/shared/efisp_scan.h` is no longer load-bearing. `gbl-pack` currently refuses to emit overlays whose `cached_abl` contains the UTF-16 efisp pattern — this becomes a *warning* (printed once, doesn't abort) for the first release after the rework, then removed in the release after that. Defense-in-depth is preserved via the hook itself.
- `tests/host/` cases that assert patch1 application or the post-patch efisp invariant: deleted / replaced with BlockIo-hook-behavior tests.

### 7. `DynamicPatchLib` rename + scope reshuffle

| Today | Rework |
|---|---|
| `mode_1/mode_1.c`, `mode_1/Signatures.h` | `libavb_bypass/libavb_bypass.c`, `libavb_bypass/Signatures.h` |
| `kMode1Patches[]`, `kMode1PatchesCount` | `kLibavbBypassPatches[]`, `kLibavbBypassPatchesCount` |
| `SCOPE_MODE_1` | `SCOPE_LIBAVB_BYPASS` |
| `PatchScope.h: EnsureInitScoped (GBL_OEM oem, int include_mode1)` | `EnsureInitScoped (GBL_OEM oem, int include_libavb_bypass)` |
| `universal/universal.c` (patch1 only) | `universal/universal.c` (empty patch table, kept for future) |
| `oem/oneplus_canoe.c` (patch7) | `oem/oneplus_canoe.c` (unchanged content; documented as capability-keyed) |

`PatchTable.c::InitAggregate` (EDK-II compile-time path) replaces the
`GBL_MODE`-driven include with two capability flags:

```c
#if (GBL_LIBAVB_BYPASS == 1)
  // append kLibavbBypassPatches
#endif
#if (GBL_OEM == GBL_OEM_ONEPLUS)
  // append kOemOneplusPatches
#endif
```

`EnsureInitScoped` (host-tool runtime path) is unchanged in structure;
only the second parameter is renamed.

**On-device dynamic-patch fallback.** When `cached_abl` is absent and
`BootFlow.c::RunDynamicPatchOnSlotAbl` patches the slot ABL at boot, the
fallback must apply *only the patches consistent with the active
manifest* — otherwise a mode-0 manifest would receive libavb-bypass
patches from a single-EFI build that compiles every group in. The
rework moves the dynamic-patch path onto `EnsureInitScoped` (previously
host-only), parameterized by the manifest:

```c
DynamicPatchLib_EnsureInitScoped(
    gManifest.Oem,                       // GBL_OEM_NONE | GBL_OEM_ONEPLUS
    gManifest.WantLibavbBypass           // 0 | 1
);
DynamicPatch_Apply(Pe, PeSize, &Result);
```

`EnsureInitScoped` therefore stops being `__HOST_BUILD__`-gated and is
compiled in both contexts. The compile-time `EnsureInit` is retained
only for builds that pre-pin everything (used by the EDK-II default
path when no manifest is available; falls back to "all caps off,"
equivalent to mode-0). Cached-ABL path is unaffected — patches were
already applied at host time.

### 8. `abl-patcher` CLI

- `--oem <id>` — unchanged (already capability-orthogonal in the C tool;
  this rework just makes the host wrapper agree).
- `--no-mode1` — kept as-is for compat (loud-but-not-fatal `argv`
  presence). Add `--no-libavb-bypass` as the canonical spelling; `--no-
  mode1` becomes an alias that prints a deprecation note to stderr. The
  polarity-rename (flipping to opt-in) is **not** in scope (would force
  `mode-1-install.sh` to start passing an explicit flag, expanding
  blast radius).

### 9. Host packaging — `efisp-package.py`

Two real behavior changes:

**(a) `--oem` becomes orthogonal to `--mode`.** The old gate
(`efisp-package.py:129-130` "only valid for --mode 2") is removed. The
patch-step composition is:

```python
patch_argv = [patch, "--in", extracted, "--out", patched]
if args.mode != "1":
    patch_argv.append("--no-libavb-bypass")
if args.oem:
    patch_argv += ["--oem", args.oem]
```

**(b) New `--manifest` packing step and one base EFI.** `gbl-pack` learns
a `--manifest <bits>:<oem>` argument that emits a `GBLP1_TYPE_MANIFEST`
entry. `efisp-package.py` derives manifest bits from `--mode`:

```
mode 0  →  WantFakelockHook=0, WantMode2Spoof=0    (no manifest needed, but emitted for inspect)
mode 1  →  WantFakelockHook=1, WantMode2Spoof=0
mode 2  →  WantFakelockHook=0, WantMode2Spoof=1
```

The wrapper concatenates **the** base EFI (single binary) + GBLP1 overlay
(cached_abl + optional mode2_profile + manifest) → `<out>.efi`.

CLI surface stays: `--mode {0,1,2}` is the preset front-door; advanced
users get capability flags later if a real need surfaces.

### 10. Install ZIP — base EFI selection collapses

`zip/modes/mode-{0,1,2}-install.sh` no longer pick a per-mode base EFI:

```sh
# was: M_EFI=mode-N.efi
M_EFI=gbl-chainload.efi   # single base
M_MANIFEST_BITS=...       # per-mode capability bits
M_OEM=...                 # per-mode (mode-2 sets it from detect_oem)
```

`zip/modes/install-common.sh::build_payload` adds `--manifest
$M_MANIFEST_BITS:$M_OEM` to its `gbl-pack` invocation. `detect_oem` moves
from `mode-2-install.sh` into `install-common.sh` as shared infra (with
one caller today, `mode-2-install.sh`'s `mode_prepare`).

`MODE_TOOLS` lists per mode are unchanged.

### 11. Build system — one EFI

- `GblChainloadPkg.dsc` loses `DEFINE GBL_MODE` and the `-DGBL_MODE` flag.
  The single EFI is built with **patch-group compile-time flags set to
  the union of shipped capabilities** so all patch groups are linked in
  and `EnsureInitScoped` can choose runtime: `-DGBL_LIBAVB_BYPASS=1
  -DGBL_OEM=ONEPLUS`. There are **no compile-time flags for
  `GBL_FAKELOCK_HOOK` or `GBL_MODE2_SPOOF`** — those are pure runtime
  manifest bits driving `if (gManifest.WantX)` gates; the corresponding
  hook mutation code (`Mode1Overlay`, `Mode2Rewrite`) is always
  compiled into the binary.
- `scripts/build-cross-tools.sh`, `scripts/build-recovery-tools.sh`,
  release-workflow scripts: any "per mode" loop collapses to a single
  build pass.
- `dist/` layout: `dist/gbl-chainload.efi` replaces `dist/mode-{0,1,2}.efi`.
  ZIP packaging stages this single base.

### 12. `gblp1-inspect`

Learns to pretty-print the new manifest entry:

```
GBLP1 manifest:
  schema_version: 1
  capabilities:
    libavb_bypass: yes  (advisory — patches landed in cached_abl)
    fakelock_hook: yes
    mode2_spoof:   no
  oem:           oneplus
```

When the manifest is absent, prints `manifest: (absent — effective
mode-0 / observation default)`.

## Architecture changes summary

```
                  ┌───────────────────────────────────────────────┐
                  │           Single gbl-chainload.efi            │
                  │ (was three: mode-0.efi, mode-1.efi, mode-2.efi)│
                  └────────────────────┬──────────────────────────┘
                                       │
                              GblPayload_LoadManifest
                                       │
                                       ▼
                          gManifest = { caps, oem }
                                       │
                ┌──────────────────────┼──────────────────────────┐
                ▼                      ▼                          ▼
       Mode2_SetProfile if      ProtocolHook_InstallAll      (Mode1Overlay /
       WantMode2Spoof           — required gates by caps     Mode2Rewrite
                                                              mutation paths
                                                              gate per-call on
                                                              gManifest.WantX)

                          BlockIoHook (unconditional)
                                       │
                                       ▼
                          EFISP read/write → EFI_NO_MEDIA
                          (retires patch1; pre-LoadImage install)
```

## Testing strategy

- **Unit/host:** new `tests/host/` case for the manifest parser:
  absence → all-zero caps; present + valid → bits round-trip; present
  + unknown bits → reject; bad schema_version → reject; bad reserved
  → reject.
- **Patch engine:** existing `tests/host/088_patch7_multi_abl.sh` and
  any patch6/patch10 cases continue to run, against renamed
  `libavb_bypass/` paths. Cases asserting patch1 or the post-patch
  efisp invariant are removed.
- **Host wrapper:** `tests/host/` case asserting `efisp-package.py`
  argv composition for each (mode × oem-present/absent) combination
  hits the right `abl-patcher` flags AND emits a manifest entry with
  the right bits.
- **On-device smoke:** the existing infiniti staged-load + `oem
  boot-efi` test loop runs against the single-EFI build for each preset
  (mode 0/1/2) and confirms behavior parity. The user owns this device
  and runs the test manually; brick-recovery via VolUp + `fastboot
  flash efisp` is the safety net (CLAUDE.md §"Safety: never flash
  non-HLOS images" allows EFISP under user-driven recovery).
- **BlockIo EFISP gate:** on-device confirmation that second-stage ABL
  does not re-enter gbl-chainload via EFISP probe after the patched
  ABL boots. Failure mode is overt (recursion → watchdog brick), so
  one positive boot is the test.

## Rollout / risk

- **Single PR.** The rename and the runtime-gating moves are
  mechanical-but-correlated; staging them across PRs creates a "half-
  renamed" state that's worse than a single atomic change.
- **Pre-merge gate:** all three preset-equivalent on-device boots
  (mode 0/1/2) verified by the user against the single-EFI build before
  merge.
- **Brick risk:** mitigated by (a) `Entry.c`'s VolUp window installing
  pre-hook, (b) `fastboot stage + oem boot-efi` test loop never
  touching persistent EFISP, (c) the existing release artifact remains
  flashable as a rollback. The hook itself is the safest possible
  EFISP-recursion stop: the second-stage ABL gets `EFI_NO_MEDIA` and
  falls back to its normal boot path.
- **Audit weakening:** mitigated by manifest validation (unknown bits
  reject, schema_version pinned, reserved must be zero) and by the
  fact that the manifest source is user-controlled host packing.

## Deferred follow-ups

- Flip `--no-mode1` polarity to `--libavb-bypass` (opt-in). Requires
  install-script changes to mode-1; out of scope for this PR.
- Move `oem/oneplus_canoe.c` content under a sub-namespace once a
  second OEM patch group appears.
- `gbl-fastboot` `oem manifest` command to print the active manifest
  from the staged overlay (read-only diagnostic).
- Migrate the on-device "is the cached ABL patched with X?" diagnostic
  to read the manifest's advisory bits instead of pattern-scanning the
  PE.

## Open questions

None at this time. All design calls above are committed to.

## File touch list (informative)

```
GblChainloadPkg/Application/GblChainload/Entry.c          (drop GBL_MODE refs)
GblChainloadPkg/Application/GblChainload/BootFlow.c       (manifest load + gate refactor)
GblChainloadPkg/Application/GblChainload/GblChainload.inf (flag rename)
GblChainloadPkg/GblChainloadPkg.dsc                       (one binary, capability flags)
GblChainloadPkg/Library/DynamicPatchLib/PatchScope.h      (param rename)
GblChainloadPkg/Library/DynamicPatchLib/PatchTable.c      (flag-driven aggregation)
GblChainloadPkg/Library/DynamicPatchLib/universal/universal.c        (delete patch1)
GblChainloadPkg/Library/DynamicPatchLib/universal/Signatures.h       (drop efisp pattern)
GblChainloadPkg/Library/DynamicPatchLib/mode_1/ → libavb_bypass/     (rename)
GblChainloadPkg/Library/DynamicPatchLib/Internal/PatchEngine.c       (drop efisp invariant)
GblChainloadPkg/Library/ProtocolHookLib/InstallAll.c                 (capability gates)
GblChainloadPkg/Library/ProtocolHookLib/VerifiedBootHook.c           (gate-by-manifest)
GblChainloadPkg/Library/ProtocolHookLib/Mode1Overlay.{c,h}           (drop #if)
GblChainloadPkg/Library/ProtocolHookLib/Mode2Rewrite.{c,h}           (drop #if)
GblChainloadPkg/Library/ProtocolHookLib/SpssHook.c                   (gate-by-manifest)
GblChainloadPkg/Library/ProtocolHookLib/QseecomHook.c                (gate-by-manifest)
GblChainloadPkg/Library/ProtocolHookLib/BlockIoHook.c                (EFISP record + EFI_NO_MEDIA)
GblChainloadPkg/Library/GblPayloadLib/PayloadParse.c                 (new entry type)
GblChainloadPkg/Library/GblPayloadLib/Internal/PayloadParse.h        (new constants)
GblChainloadPkg/Library/GblPayloadLib/GblPayload.c                   (LoadManifest)
GblChainloadPkg/Include/Library/GblPayloadLib.h                      (new API)
tools/abl-patcher/abl-patcher.c                                       (--no-libavb-bypass alias)
tools/gbl-pack/                                                       (--manifest arg)
tools/gblp1-inspect/                                                  (pretty-print manifest)
tools/shared/efisp_scan.h                                             (warning-only or removed)
scripts/efisp-package.py                                              (decouple --oem, manifest, single EFI)
scripts/build-*.sh                                                    (single build pass)
zip/modes/install-common.sh                                           (detect_oem moved here; --manifest in build_payload)
zip/modes/mode-0-install.sh, mode-1-install.sh, mode-2-install.sh    (single base EFI, manifest bits)
zip/update-tools.sh                                                   (single artifact)
tests/host/                                                           (manifest tests; remove patch1 tests)
docs/archive/project/current-state.md, re-findings.md                         (taxonomy updates)
```
