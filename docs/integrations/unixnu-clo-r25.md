# UniXNU integration on the CLO r25 core

2026-11-15: UniXNU and DarwinBoot both compile and link against
`uefi.lnx.5.0.r25-rel` with GCC5, in both DEBUG and RELEASE. This confirms
that a shared CLO core is feasible. The device owner confirmed both Darwin
startup through UniXNU and normal Android startup on fairlady using the test
image recorded below. This records startup only, not full Darwin driver or
userspace validation.

## Source revisions

| Input | Revision |
| --- | --- |
| CLO edk2 | `3b3ae27ca861001a01dbbe39ad74d5d879f3e73e` |
| GBL edk2 with the existing chainload commits | `d960ff142a08036e82801e7f88c6b3db22e0e459` |
| UniXNU | `16e592abee8a550dec8154ed1f38b5ebc4f16213` |
| DarwinBoot | `3325e352d7df68a9f9ad74832ed94a6acbbd7390` |
| BaseFdtLib, FdtLib.h, QuickSort and Linux initrd GUID source | Mu_Basecore `cfb93724a06ff86ff5ba971413368a7809ee42a5` |
| BaseFdtLib's libfdt source | `cfff805481bdea27f900c32698171286542b8d3c` |

The final application and driver were built from clean Git archives, without
the device workspace's uncommitted UniXNU changes. Both pinned UniXNU and
DarwinBoot revisions were checked as accessible on their public GitHub repos.
The Mu checkout supplied individual library sources only; all core libraries,
entry points and headers used by this build otherwise come from CLO r25.

## Compatibility changes

- Add `BaseFdtLib`, its `libfdt` sources, `FdtLib.h`, and the DEC library mapping.
- Add the standard FDT and Linux initrd GUID headers and DEC declarations.
  CLO already declares the same FDT GUID in EmbeddedPkg; the test DSC uses
  the MdePkg declaration for UniXNU.
- Add `QuickSort` to BaseLib, including its comparator type and prototype.
- Expose CLO's existing `ArmPkg/Include/AsmMacroLib.h` under the AArch64
  MdePkg include directory expected by DarwinBoot. Copying the newer Mu header
  alone fails because CLO lacks its `_ASM_TYPE` processor macro.
- Use an explicit DSC instead of UniXNU's standalone DSC, which includes the
  unavailable `MdePkg/MdeLibs.dsc.inc`. Map CLO's `BaseMemoryLibOptDxe` and
  ArmPkg compiler intrinsics; define `__FORTIFY_SOURCE` as in the GBL build.

MipiSysTLib and Brotli are checks in UniXNU's standalone build helper; this
two-component DSC successfully builds without them. No UniXNU or DarwinBoot
source edits were needed for the final build.

## Building from source

```bash
git submodule update --init edk2
scripts/build-native.sh --output dist/gbl-chainload-debug-gcc5.efi
scripts/build-unixnu.sh
```

`scripts/unixnu-sources.json` pins the UniXNU and DarwinBoot Git revisions.
The helper fetches only the UniXNU package and DarwinBoot sources into ignored
`Build/unixnu/` caches, refuses dirty caches and builds with the same `edk2/`
core as GBL. The libfdt submodule is pinned by edk2. The compatibility changes
listed above now live in edk2 and the build DSC is
`GblChainloadPkg/Unixnu.dsc`; there is no second core checkout to build.

Outputs are `Build/unixnu/RELEASE/{UniXNU,DarwinBoot}.efi` by default.
`scripts/build-unixnu.sh --debug` selects DEBUG and writes to
`Build/unixnu/DEBUG/`. GCC, aarch64 GNU binutils, Python 3, Git, make and Rust
are required by the native build helpers. No XNU kernel or FAT payload is
built or bundled by these helpers.

Package both new EFI files with your device's ABL and vbmeta inputs:

```bash
python3 scripts/efisp-package.py --bin-dir target/release \
  --mode 2 --oem oplus --abl /path/to/abl.img \
  --stock-vbmeta /path/to/vbmeta.img \
  --efi dist/gbl-chainload-debug-gcc5.efi \
  --unixnu-efi Build/unixnu/RELEASE/UniXNU.efi \
  --unixnu-loader-efi Build/unixnu/RELEASE/DarwinBoot.efi \
  --out dist/efisp-with-unixnu.efi
```

Build `target/release/gbl` first with `cargo build --release -p gbl`.
Normal Android boot remains the default; UniXNU is started explicitly with
`fastboot stage payload.fat` followed by `fastboot oem boot-unixnu` in GBL's
FastbootLib. The payload must contain `EFI/UniXNU/unixnu.toml` and its selected
boot inputs. This integration does not install UniXNUDxe or alter BootOrder.
The temporary RAM disk is detached when startup fails or UniXNU returns,
before a new download, and before FastbootLib releases its transfer buffers.

## Initial experiment provenance

Generated, ignored files are retained under `Build/unixnu-clo-r25/`:

- `clo-compat.patch`: complete experimental core patch, including libfdt source.
- `UnixnuClo.dsc`: the two-component DSC.
- `source/`: clean UniXNU package and DarwinBoot source snapshots.
- `compile-published.log`, `compile-published-debug.log`, `SHA256SUMS`.

The initial experiment used independent temporary worktrees. Its successful
compatibility changes were subsequently carried into the shared edk2 core
and the source build helper above. The saved experiment is optional local
provenance, not a build input or a tracked source dependency.

The raw EFI outputs are in `dist/unixnu-clo-r25/{DEBUG,RELEASE}/`:
`UniXNU.efi` is 98,304 bytes; `DarwinBoot.efi` is 36,864 bytes. Their PE types
are AArch64 EFI application and EFI boot-service driver, respectively.

## Fairlady test image

`dist/efisp-payload/fairlady-PLZ110-16.0.5.701-clo-r25-unixnu-source-mode2-debug.efi`

- GBL base: `/tmp/gbl-r25-debug.efi`, built from the rebased chainload tree.
- Embedded components: this experiment's RELEASE UniXNU and DarwinBoot.
- Cached ABL: `abl_b-PLZ110_16.0.5.701(CN01).img`.
- Mode-2 profile input: `fairlady/gbl/vbmeta_a.img`.
- Size: 1,573,496 bytes, within the 3 MiB EFISP partition.
- SHA256: `423efaac5f348dbe939ccb3e551eb5e6724b8e65241fd4cb7ea3e90e56a221db`.

`gbl inspect` passes: header CRC, six entry hashes and footer are valid. The
device owner reported successful Darwin and Android startup with this image.
Stage, boot and flashing remain manual device operations. Subsequent builds
are distinct artifacts; this recorded hash identifies the tested image.

## Host verification during integration

The focused `cargo test -p gblp1 -p pe-utils` suite passes, including the
six-entry container, prefixed FFI scan, missing-entry and corruption cases.
The native GBL build and both UniXNU build targets pass with the shared core.
The existing `085_efisp_package.sh` and `094_gbl_pack_manifest.sh` host
regressions also pass.

The broader `cargo test --workspace` run is blocked by the unhydrated
`tests/fixtures/recovery/grafted-recovery.img` Git LFS fixture: the local file is a
134-byte pointer rather than the 100 MiB image. Three avb-parse parity tests
fail to find its AVB footer. This fixture was not modified for the feature.
