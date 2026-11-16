# gbl-chainload

English | [简体中文](README.zh-CN.md)

An AArch64 UEFI chainloader for devices with a working Qualcomm GBL / EFISP loading mechanism. It patches ABL in memory, installs targeted protocol hooks, and hands off to the patched ABL to boot Android. It also provides GBL FastbootLib and optional UniXNU EFI boot support.

This repository continues development from [1vivy/gbl-chainload](https://github.com/1vivy/gbl-chainload). Firmware uses the repository's pinned CLO edk2 core; the host tools and patch engine are written in Rust.

## Read Before Use

> **This is experimental boot-chain tooling, not a universal flashable package or a brick-prevention guarantee.** Mismatched ABL, vbmeta, or firmware can cause boot failures, 900E, loss of fastboot access, or data loss. Back up your device and establish a recovery method first.

- Prefer temporary loading with `fastboot stage` for initial validation. Do not directly flash boot-chain partitions or perform a real relock to test fakelock.
- Temporary loading means the candidate EFI is not written to its partition; **it does not make the entire boot process read-only or risk-free**. ABL, Android, and fastboot commands can still modify device state.
- mode-1 / mode-2 are not a real bootloader relock and do not guarantee acceptance by every app or hardware-attestation check.
- patch12/13 can take effect only after GBL runs and loads the patched ABL. They cannot repair failures before GBL starts.
- Static patch matches and passing host tests do not constitute hardware boot validation.

This README describes the current scripts and code. Historical experiments, device differences, and reverse-engineering findings live in the [project notes](docs/project/README.md). [VERSION](VERSION) is the version source of truth.

## Boot Flow and Compatibility

Normal Android startup follows this path:

```text
Device UEFI / GBL loading entry
  └─ gbl-chainload EFI + GBLP1 payload
       ├─ Prefer the pre-patched ABL cached in the payload
       ├─ If unavailable, try extracting and patching the active-slot ABL
       ├─ Install protocol hooks according to the payload manifest
       └─ Hand off to patched ABL → Android
```

When ABL extraction, required patching, or handoff fails, the program attempts to return to GBL FastbootLib as a recovery surface. This is not a promise that every failure is recoverable.

The project primarily targets OnePlus / Oppo and other Oplus devices. The current ABL test pool has 13 deduplicated images covering OnePlus 15 (infiniti), OnePlus 15T (fairlady), OnePlus Ace 6T (macan), and selected Xiaomi / Redmi builds. **This is patch-regression coverage, not a universal supported-device list**; a usable GBL / EFISP entry is also required on the device.

See the [test-image notes](tests/images/README.md) for provenance, version evidence, and test usage. Do not use another device's fixture ABL on your own hardware.

## Operating Modes

**One base EFI supports every mode.** The GBLP1 manifest written during packaging selects the mode; firmware is no longer compiled separately for each mode.

| Mode | Behavior |
| --- | --- |
| mode-0 | Enables neither fakelock nor profile spoofing; common preservation policies and the selected packaging-time ABL patches still apply. |
| mode-1 | Temporary protocol-level fakelock presents a locked view to relevant interfaces; persistence policies prevent the spoofed locked state from being committed to RPMB. |
| mode-2 | Custom-ROM mode modifies TA payloads at QSEE / SPSS boundaries using a vbmeta-derived profile; ABL internally retains the real unlocked state. |

mode-0 is not an unmodified observation boot, and mode-1 / mode-2 do not guarantee compatibility with arbitrary mixed firmware. Recheck ABL, vbmeta, and the mode-2 profile when changing ROMs or installing an OTA.

Mode-2 validates its profile before loading or dynamically patching ABL. A missing or invalid profile aborts chainload and returns to GBL fastboot instead of continuing with a locked/green handoff but no TA profile. Each attempt clears stale profile state and warnings; failed handoffs also clear the profile. QSEE profile rewrites require a handle confirmed by a successful `keymaster` / `keymaster64` start; command numbers alone never authorize a rewrite, and unconfirmed or other TA handles pass through unchanged. Mode-1 persistence policies are unchanged by these checks.

### Patches and OEM Selection

Mode selection and OEM selection are independent:

| Patch | Scope | Purpose |
| --- | --- | --- |
| patch6 / patch10 | Cross-OEM group | Modify ABL fastboot lock-state gates and libavb verification-result handling. |
| patch11, including 11a / 11b / 11c | mode-2 only, cross-OEM | Publish locked / green handoff state and preserve required vbmeta digest parameters with verification-disabled vbmeta. |
| patch7 | Oplus, host packaging only | Skip ABL's unlocked warning screen and associated wait. |
| patch12 | Oplus | Keep A/B retry counting and slot switching while bypassing the analyzed fastboot-refusal, reset, and dead-spin branches. |
| patch13 | Oplus | Make the stored unbootable flag inert for the relevant bootability decisions in this ABL run; leave GPT writes unchanged and do not promise to clear the on-disk flag. |

- Use `--oem oplus` when packaging for Oplus to enable patch7/12/13.
- Omitting `--oem` or using `--oem none` skips all three on the host; they are not attempted or counted as `MISS`.
- For Xiaomi / Redmi, omit the option or use `--oem none`. `oneplus` remains a deprecated alias for `oplus`.
- Firmware fallback from the active-slot ABL enables patch12/13 only when both Oplus-specific marker strings are present. Patch7 remains host-only.
- patch12/13 remain optional and idempotent. Mode-2 packaging requires all three parts of patch11; a missing part aborts output generation.
- patch11a/11b/11c are enabled together only for mode-2. mode-0 / mode-1 skip them entirely; mode-1 uses its own protocol hooks rather than patch11. Direct `gbl patch` defaults to mode-0; use `gbl patch --mode 2` to select patch11. Firmware fallback reads the manifest before patching and follows `WantProfileSpoof`.

### Preservation and mode-1 Persistence Policies

All modes install the common preservation baseline: suppress the three handled TZ soft-fuse / rollback-update SCM calls and swallow writes to matched `oplusreserve1` / `opporeserve1` partitions, including token-zeroing writes. These are targeted hooks, not a general read-only policy or a guarantee that downgrading firmware is safe.

Under mode-1, VB `WRITE_CONFIG` is **healed toward unlocked and forwarded**, not simply dropped. VB device reset, OplusSec `write_rpmb_boot_info` (`0x0A`), and KeyMaster `WRITE_KM_DEVICE_STATE` (`0x203`) are suppressed on the corresponding intercepted paths. The intent is to keep fakelock temporary rather than persist a spoofed locked record. This policy is not a per-SoC whitelist; device observations and remaining uncertainties are recorded in the [reverse-engineering findings](docs/project/re-findings.md).

Do not relock or switch slots merely to test these hooks. Existing captures can be checked for `write-swallow`, `token-zero-write`, `healed->unlocked`, and `WRITE_KM_DEVICE_STATE` events; their presence alone is not a complete safety proof.

### patch11c and vbmeta Handoff Consistency

Stock libavb's verification-disabled path can omit `androidboot.vbmeta.{hash_alg,size,digest}` while relying on an orange userspace view. After patch11a/11b report locked / green, that omission can break first-stage mounting. Patch11c removes the disabling branch so the normal handoff publishes those parameters.

`--experimental-vbmeta-cmdline` is now a deprecated no-op because 11c is part of the mode-2-only patch11. Mode-2 packaging automatically passes `--require-patch11`; explicitly requesting that flag with mode-0 / mode-1 is rejected. This does not make mismatched stock/custom AVB chains valid; 11c also reaches the managed-verity helper normally bypassed by the disabled path. Validate the complete image combination rather than treating a patch match as proof of compatibility.

## Building from Source

### Native Linux Build

Install Git, Python 3, make, a host C/C++ build environment, Rust / Cargo, and an AArch64 GNU cross-toolchain providing `aarch64-linux-gnu-gcc` and matching binutils. See the [Dockerfile](docker/Dockerfile) for additional EDK2 BaseTools dependencies.

```bash
git clone https://github.com/Osilvfe/gbl-chainload.git
cd gbl-chainload
git submodule update --init edk2
rustup target add aarch64-unknown-none

cargo build --release -p gbl
scripts/build-native.sh
```

Default outputs:

- `target/release/gbl`: the host multicall tool, with `unwrap`, `patch`, `pack`, `inspect`, `mode2`, `avb`, and `commit` subcommands.
- `dist/gbl-chainload-debug-gcc5.efi`: the base EFI with debug logging, **without a device-specific packaged payload**.

To disable GBL debug output:

```bash
scripts/build-native.sh --release --output dist/gbl-chainload.efi
```

The native script always uses EDK2's `RELEASE` target. Its default debug name means `GBL_DEBUG=1`; `--release` changes that to `0`. Do not infer all optimization settings from a filename, or treat hardware results from different toolchains or logging configurations as interchangeable.

`cargo` builds the Rust tools and static libraries; the final EFI still requires EDK2 linking. `cargo build` alone does not produce the complete firmware.

### Docker Build

Docker is an optional way to standardize the toolchain for CI and cross-platform development, not a runtime dependency. A configured native Linux host can use the workflow above directly. Containerizing a build does not itself guarantee byte-for-byte reproducibility or solve device boot failures.

With Docker available, use the unified build entry:

```bash
scripts/build.sh --debug --no-recovery
```

This example emits `dist/gbl-chainload-debug.efi` and `dist/host/gbl`. Remove `--no-recovery` to also build the Android recovery tool. Firmware and recovery builds run in the container; the host-tool phase still requires native Cargo, or can be skipped with `--no-host`. `--auto`, `--debug`, and `--verbose` control entry policy and logging; **there is no build-time `--mode` option**.

The script clears `Build/`. Build GBL before UniXNU if you need both, to avoid deleting freshly built components. For the later packaging examples, Docker users should use `--bin-dir dist/host` and `--efi dist/gbl-chainload-debug.efi` instead of the native output paths, and run `inspect` through `dist/host/gbl`.

## Packaging a Device-Specific EFISP

The examples below use native build outputs. Supply the target device's corresponding ABL and the vbmeta used to derive its profile; do not reuse inputs across devices.

For Oplus mode-2:

```bash
python3 scripts/efisp-package.py \
  --bin-dir target/release \
  --abl /path/to/stock_abl.img \
  --mode 2 \
  --oem oplus \
  --stock-vbmeta /path/to/stock_vbmeta.img \
  --efi dist/gbl-chainload-debug-gcc5.efi \
  --out dist/efisp-payload/device-mode2-debug.efi

target/release/gbl inspect dist/efisp-payload/device-mode2-debug.efi
```

- For mode-0 / mode-1, change `--mode` to `0` / `1` and remove `--stock-vbmeta`. Keep `--oem oplus` for the Oplus-specific patches.
- mode-2 requires `--stock-vbmeta` and automatically enables `--require-patch11`.
- The **packaging script** forwards its mode to `gbl patch --mode`; it is not a base firmware build option.
- The script unwraps, patches, derives the profile, and concatenates the payload; **it does not flash your device**.
- `inspect` checks container structure and integrity, not hardware compatibility. Also verify that the final file fits the target EFISP partition.

See the [host-tool documentation](tools/README.md) for more options.

Existing EFISP images may already contain patch11 from the old all-mode policy. This change does not undo those cached ABL edits; rebuild and repackage mode-0 / mode-1 images from stock inputs to obtain the new behavior.

## Temporary Loading and the Two Fastboots

**GBL FastbootLib and the stock ABL fastboot are different implementations.** Seeing a device in `fastboot devices` does not identify GBL, and `adb reboot bootloader` is not a universal way to enter GBL FastbootLib.

The following workflow requires **an already working GBL FastbootLib with `oem boot-efi` support**. Establishing the initial GBL / EFISP entry is device-specific; there is no universal installation sequence for every model.

Query the running build, then load a candidate:

```bash
fastboot getvar gbl-chainload_build
fastboot stage dist/efisp-payload/device-mode2-debug.efi
fastboot oem boot-efi
```

Base EFI entry policy:

| Build policy | Key-window timeout | Volume Up |
| --- | --- | --- |
| Default, without `--auto` | Continue into patched ABL, normally toward Android | Enter GBL FastbootLib |
| Host-gated build with `--auto` | Wait in GBL FastbootLib for host commands | Immediately continue into patched ABL |

To keep operating in a newly staged candidate's fastboot, enter FastbootLib according to that candidate's policy. `fastboot oem escape` leaves GBL FastbootLib and continues through the patched ABL path.

**Staging does not save the candidate to EFISP.** After a reboot, the device loads its installed firmware / EFISP again; do not assume the staged candidate is still running.

### Diagnostic Queries

GBL publishes build and logging metadata, plus lightweight vbmeta diagnostics:

```bash
fastboot getvar gbl-chainload_version
fastboot getvar gbl-chainload_date
fastboot getvar gbl-chainload_auto
fastboot getvar gbl-chainload_debug
fastboot getvar gbl-chainload_verbose
fastboot getvar unlocked
fastboot getvar oem-unlock-allowed
fastboot getvar vbmeta:capabilities
fastboot getvar vbmeta:slot
fastboot getvar vbmeta:warning
fastboot getvar vbmeta:boot:status
fastboot getvar vbmeta:boot:descriptor-type
```

The per-partition form is `vbmeta:<partition>:status` / `vbmeta:<partition>:descriptor-type`. These query results and menu warnings are diagnostic hints, not full AVB or attestation validation. Stock ABL fastboot does not necessarily expose them. Known menu-warning and installer limitations are recorded in the [upstream issue notes](docs/project/upstream-issues.md); lock-state and partition-write commands are deliberately excluded from this trial workflow.

### Temporarily Booting boot.img

In GBL FastbootLib, the normal command retains the DTBO processing path:

```bash
fastboot boot /path/to/boot.img
```

To skip DTBO, use the separate device-side command:

```bash
fastboot stage /path/to/boot.img
fastboot oem boot-nodtbo
```

`boot-nodtbo` skips reading the DTBO partition and applying its overlay, and omits `androidboot.dtbo_idx`. The base DTB still comes from the boot / vendor_boot path, and AVB still runs. This does not guarantee that an arbitrary kernel image can boot.

### Current Boot-Path Fixes and Historical Tools

Current source builds already skip the verified-boot warning UI on GBL's temporary boot path, initialize platform data through `BoardInit()` instead of hard-coded fairlady IDs, and carry the LibUfdt overlay allocator reset/free fixes. GBL's own boot path is separate from the patched ABL path: skipping its warning UI is not disabling AVB, and a normally booting Android installation does not prove that every `fastboot boot` image will work.

The older [binary-patch helper](scripts/gbl-fastboot-boot-fix.py) targets specific historical EFI layouts. It is not a required post-build step and must not be applied blindly to current firmware. Historical offsets, observations, and diagnostic traces remain in the [reverse-engineering findings](docs/project/re-findings.md).

## Optional UniXNU Integration

The integrated components are the UniXNU EFI application and DarwinBoot EFI driver, built against the same CLO edk2 core as GBL. No nested Mu firmware or FD is needed.

Build the components from pinned source revisions:

```bash
scripts/build-unixnu.sh
```

Then package both components into EFISP:

```bash
python3 scripts/efisp-package.py \
  --bin-dir target/release \
  --abl /path/to/stock_abl.img \
  --mode 2 --oem oplus \
  --stock-vbmeta /path/to/stock_vbmeta.img \
  --efi dist/gbl-chainload-debug-gcc5.efi \
  --unixnu-efi Build/unixnu/RELEASE/UniXNU.efi \
  --unixnu-loader-efi Build/unixnu/RELEASE/DarwinBoot.efi \
  --out dist/efisp-payload/device-unixnu-mode2-debug.efi
```

[unixnu-sources.json](scripts/unixnu-sources.json) pins the source revisions. Source caches and outputs live in ignored `Build/unixnu/` directories. The helper does not build an XNU kernel or FAT payload, and this feature does not track prebuilt EFI binaries or device payloads. `scripts/build-unixnu.sh --debug` selects EDK2 `DEBUG` and writes to `Build/unixnu/DEBUG/`; adjust both component paths when packaging those outputs.

Once inside GBL FastbootLib containing these components:

```bash
fastboot stage /path/to/payload.fat
fastboot oem boot-unixnu
```

`payload.fat` must be a raw FAT filesystem image containing `EFI/UniXNU/unixnu.toml` and the boot files referenced by that configuration. GBL exposes it as a read-only RAM disk and starts the embedded UniXNU. **Normal startup, key handling, and timeout do not automatically select UniXNU; Android remains the normal default target.**

Android and Darwin startup have been reported on fairlady; this is not validation of every Darwin driver or userspace component. See the [UniXNU integration notes](docs/notes/unixnu-clo-r25.md) for implementation details and the tested image's provenance.

## Logging and Troubleshooting

Common firmware logs reside on the `logfs` partition:

- `GblTrace.txt`: flushed after each milestone to locate pre-handoff failures; recreated on each LogFs initialization and capped at 48 KiB.
- `UefiLog1.txt`: the platform UEFI log.
- `UefiLogSaved*.txt`: rotated historical logs; distinguish them by build metadata and boot sequence.

On an already rooted Android system, confirm the actual partition path and existing mounts before exporting logs read-only. If `logfs` is already mounted, use that mount instead of repeating these operations:

```bash
adb shell 'su -c "mkdir -p /data/local/tmp/gbl-logfs && mount -t vfat -o ro /dev/block/by-name/logfs /data/local/tmp/gbl-logfs"'
adb pull /data/local/tmp/gbl-logfs ./gbl-logfs
adb shell 'su -c "umount /data/local/tmp/gbl-logfs"'
```

Save failed-boot logs promptly: later boots or initialization may overwrite them. Review logs for device identifiers and other private information before publishing them. Standard UEFI logs depend on platform buffering and flush timing; an early hang may leave no final UEFI record, which is why `GblTrace.txt` exists.

### Developer Logging

Use the API in [GblLog.h](GblChainloadPkg/Include/Library/GblLog.h):

| API | Configuration | Intended use and output |
| --- | --- | --- |
| `Print(L"...")` | Always enabled | Essential user prompts and fatal errors; visible on screen. |
| `GBL_INFO("...")` | `GBL_DEBUG=0` | Semantic events through the platform status-code logging path, without normal screen output. |
| `GBL_INFO("...")` | `GBL_DEBUG=1` | The same semantic events through `AsciiPrint`, visible on screen. |
| `VERBOSE("...")` | `GBL_VERBOSE=0` | Compiled out. |
| `VERBOSE("...")` | `GBL_VERBOSE=1` | High-frequency raw traces through `AsciiPrint`. |

Use `GBL_INFO` for mutations, swallowed operations, relevant intercepted-command outputs, and boot milestones. Use `VERBOSE` for pass-through queries, raw dumps, and scan iterations; do not bury correctness evidence in verbose-only logs. Keep unnecessary protocol-internal marshalling traces out of both tiers, and retain `DEBUG_ERROR` / `DEBUG_WARN` for unconditional error/warning paths.

`GBL_INFO` / `VERBOSE` take CHAR8 format strings without an `L` prefix. Use `%a` for ASCII string arguments; convert CHAR16 strings explicitly when needed. Platform support, log masks, and flush timing still determine what reaches persistent UEFI logs. Avoid flooding the log ring with raw output.

## Development and Regression Tests

Useful focused checks:

```bash
cargo test -p patch-engine --features host
bash tests/host/088_patch7_multi_abl.sh
bash tests/host/097_patch11_multi_abl.sh
bash tests/host/098_patch12_patch13_oem.sh
bash tests/host/099_mode2_runtime.sh
```

`098` checks all fixtures in mode-0 / mode-1 / mode-2 on both host and firmware engine paths: patch11 is absent in modes 0/1 and present in mode 2, while OEM selection, vendor detection, the 6+3 patch12/13 instruction rewrites, idempotency, and unchanged GPT write sites are also verified.

`099` exercises the firmware C boot flow and QSEE wrappers with mocked firmware services: profile failure aborts before ABL access, retries clear stale state, and only confirmed KeyMaster handles receive mode-2 rewrites. It does not replace device testing.

Full-suite prerequisites include real Git LFS recovery images, fetched with `git lfs pull`, and mode-2 parity golden files generated by `082_mode2_profile_parity.sh` with its derive-test prerequisites, including `avbtool`. Fetch the real fixtures and prepare those references before interpreting full-suite results: an LFS pointer is not an image, and missing prerequisite files alone do not establish a patch regression.

### Repository Layout

| Path | Contents |
| --- | --- |
| `GblChainloadPkg/` | EFI entry, boot flow, protocol hooks, and payload handling |
| `GblChainloadPkg/Library/ProtocolHookLib/` | Common `UniversalBaseline.c`, mode-1 `FakelockOverlay.c`, and mode-2 `ProfileOverlay.c` |
| `crates/patch-engine/` | Rust patch engine; always-on cross-OEM patches in `abl_permissive/`, patch11 in `mode2/`, Oplus patches in `oem/oplus/` |
| `tools/gbl/` | Host / recovery multicall tool |
| `edk2/` | CLO-derived edk2 submodule pinned to a specific commit |
| `scripts/` | Build, packaging, and development helpers |
| `tests/images/` | Deduplicated ABL and other test fixtures |
| `docs/project/` | Project state, reverse-engineering findings, decisions, and upstream issue notes |

### Releases and Documentation

Daily development and CI use `main`. Releases use an existing `release-X.Y.Z` tag matching `VERSION`; the workflow builds that commit and creates a draft for review. Historical `v...` tags do not trigger the current release workflow.

The two READMEs use the same section order and executable examples. When changing behavior, update both; keep detailed historical experiments in project notes rather than presenting them as current setup steps.

See [LICENSE](LICENSE) for this project's license. Third-party components retain their own licenses.
