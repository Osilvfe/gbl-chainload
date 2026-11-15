# tests/images — ABL fixture pool for the dynamic-patch harness

Tracked location for ABL fixtures consumed by `tests/patches/test_patch*` and
`tests/042_dynamic_patch_harness.sh`. (Top-level `images/` is gitignored —
device-specific stock OTA dumps stay local; the fixtures in this directory
are the curated test pool.)

## Naming convention

`<phone>-<codename>[-<region>][-<build-tail>]-abl.img` for raw ABL FV wrappers.
Omit the build tail when no reliable version is available; retain a region
suffix when regional binaries differ. Examples:

- `op15-infiniti-201-abl.img` — OnePlus 15 (infiniti) build 16.0.7.201
- `op15-infiniti-703-abl.img` — OnePlus 15 (infiniti) build 16.0.5.703
- `op15t-fairlady-201-abl.img` — OnePlus 15T (fairlady)
- `opace6t-macan-cn-702-abl.img` — OnePlus Ace 6T (macan), China, build tail 702
- `xi17-pudding-44-abl.img` — Xiaomi 17 (pudding), build 3.0.44.0
- `xi17pro-pandora-45-abl.img` — Xiaomi 17 Pro, build 3.0.45.0
- `xi17ultra-nezha-global-abl.img` — Xiaomi 17 Ultra global, version unknown

Extracted PEs (post-unwrap) sit alongside as
`<phone>-<codename>[-<region>][-<build-tail>]-LinuxLoader.efi` (`.efi` extension), produced
by the FV→PE extractor (`scripts/extract-pe-from-fv.sh` / `gbl unwrap`, the
`gbl` multicall subcommand that replaced the standalone `fv-unwrap` C tool
in PR2 Task 8).

## ablrepo import (2026-11-15)

Eleven supplied `ablrepo/<directory>/abl.img` files yielded seven new fixtures.
That import brought the pool to eleven unique raw ABLs and eleven unique
unwrapped PEs.
All supplied `abl.sha256` sidecars were checked. The raw images are preserved
byte-for-byte, including partition padding; extracted and patched outputs
stay in ignored build/test directories.

| Added fixture | Source directory | Device | Version evidence |
| --- | --- | --- | --- |
| `opace6t-macan-abl.img` | `PLR110` | OnePlus Ace 6T / 15R | Unknown; omitted |
| `op15t-fairlady-abl.img` | `PLZ110` | OnePlus 15T | Unknown; omitted |
| `redmik90promax-myron-23-abl.img` | `myron` | Redmi K90 Pro Max | `abl_version.txt`: 3.0.23.0 |
| `xi17ultra-nezha-abl.img` | `nezha` | Xiaomi 17 Ultra, China | Unknown; omitted |
| `xi17ultra-nezha-global-abl.img` | `nezha_global` | Xiaomi 17 Ultra, global | Unknown; omitted |
| `xi17pro-pandora-45-abl.img` | `pandora` | Xiaomi 17 Pro | `abl_version.txt`: 3.0.45.0 |
| `xi17promax-popsicle-45-abl.img` | `popsicle` | Xiaomi 17 Pro Max | `abl_version.txt`: 3.0.45.0 |

Deduplication compares both raw-image SHA256 and unwrapped-PE SHA256 against
the existing pool and other imports:

- `CPH2841`, `OPD2513`, `PLR110` and `PMA120` are byte-identical raw images
  (`0d8aa124c40348362108109890cdc25a4f3cd8f7351c1007e15d6fdbb6ed7c5f`).
  Keep only the `PLR110` representative; the other source labels correspond
  to Find X9 Ultra global, OnePlus Pad 3 Pro, and Find X9 Ultra satellite
  edition, respectively. These aliases record the supplied directory labels.
- `pudding` is byte-identical to existing `xi17-pudding-44-abl.img`
  (`2566f645be236a6adc886bbf937914765bd4c24b1bf4d19241574f70295e7056`).
- `nezha` and `nezha_global` differ in both raw-image and PE SHA256, so both
  are retained. Their embedded security patch date is not treated as a
  firmware version. The new Oplus binaries likewise do not match the known
  locally named builds, so no version is inferred from those filenames.

Device naming was cross-checked against
[MobileModels](https://github.com/KHwang9883/MobileModels/tree/65a6f7b7cd4d82de76f56d37abf241ab2c20c1c0/brands)
and [xiaomi_devices](https://github.com/XiaomiFirmwareUpdater/xiaomi_devices).
Fixture coverage measures binary patch matching; hardware boot validation
is recorded separately.

## Additional local samples (2026-11-15)

Two additional samples from `/home/osilvfe/Documents/giao/` bring the pool
to thirteen unique raw ABLs and thirteen unique unwrapped PEs. Both were
checked against the full pool using raw-image and unwrapped-PE SHA256, then
copied byte-for-byte, preserving any padding.

| Added fixture | Source relative to `giao/` | Device / region | Version evidence |
| --- | --- | --- | --- |
| `op15t-fairlady-701-abl.img` | `fairlady/abl_b-PLZ110_16.0.5.701(CN01).img` | OnePlus 15T, China | Source filename: 16.0.5.701(CN01) |
| `opace6t-macan-cn-702-abl.img` | `macanc/702abl.img` | OnePlus Ace 6T, China (user-confirmed) | Source filename: build tail 702 only; full version unknown |

These are distinct from the version-unknown fairlady and macan samples
imported from `ablrepo`; those fixtures remain unchanged.

## Which test consumes what

| Test | File pattern consumed | Coverage |
|------|-----------------------|----------|
| `test_patch1` (patch1-efisp-recursion) | `*.efi`, `*.bin`, `*.img` | byte-scan; works on raw FV |
| `test_patch7` (orange-screen) | `LinuxLoader_infiniti.efi` only | offset-specific; needs the extracted infiniti PE |
| `test_patch6` (lock-state fastboot-gate) | `*.efi` (any OnePlus/Oppo libavb PE) | string-anchored gate-bypass; needs extracted PEs |
| `test_patch10` (libavb force-AVB-success) | `*.efi` (any Qcom libavb PE) | string-anchored function entry/exit rewrite; needs extracted PEs |
| `088` patch7 cross-build gate | `op*-abl.img`, `xi*-abl.img`, `redmi*-abl.img` | unwraps in-test; patch7 applies to Oplus and misses Xiaomi/Redmi; patch6/10 apply to both |
| `097` patch11 locked-handoff gate | `*-abl.img` (all tracked ABLs, oplus + Xiaomi) | unwraps in-test; CSEL rewrite + color-table repoint + idempotency |
| `098` patch12/13 OEM scope | `*-abl.img` (all tracked ABLs) | explicit Oplus packaging + firmware vendor detection; other/unknown OEMs skip; exact 6+3 rewrites, idempotency, GPT write sites unchanged |
| `042` mandatory anchor-uniqueness | `*.efi` only | strict |
| `042` informational anchor-uniqueness | `*.bin`, `*.img` | non-fatal until extractor lands |
| `060`/`061`/`062`/`064`/`067`/`069`/`081`/`083`/`084` | `pe/infiniti-EU-16.0.5.703.efi` | unwrapped stock infiniti PE |
| `074`/`085` | `grafted-recovery.img` (LFS) | stock-vbmeta-grafted custom recovery |
| `079`/`082` | `vbmeta-infiniti-IN-16.0.7.201.img` | stock IN 16.0.7.201 vbmeta |

## Storage notes

`grafted-recovery.img` is 100 MiB and lives on Git LFS (see `.gitattributes`).
A fresh clone needs `git lfs pull` (or `git lfs install` + `git lfs fetch`) to
materialize it; without it, tests 074 and 085 SKIP. CI's `actions/checkout`
uses `lfs: true` to fetch automatically.
