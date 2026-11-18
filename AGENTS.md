# gbl-chainload — Contributor and agent guidance

This file provides repository-wide guidance for contributors and automated
coding agents, independent of any editor or assistant. More specific
`AGENTS.md` files apply within their directory trees. Explicit user requests
and higher-priority instructions take precedence.

## Safety: never flash non-HLOS images

**Do not autonomously run** any of:

- `fastboot flash <X>` where `<X>` is **not** one of
  `system`, `vendor`, `product`, `system_ext`, `odm`, `userdata`, `cache`,
  `metadata` (with optional `_a` / `_b` slot suffix).
- `fastboot oem unlock` / `fastboot oem lock`
- `fastboot flashing unlock` / `fastboot flashing lock`
- `fastboot flashing unlock_critical` / `fastboot flashing lock_critical`
- `fastboot --set-active <slot>` / `fastboot set_active <slot>`
- `fastboot erase <non-HLOS partition>`

These are device-bricking commands. To test gbl-chainload revisions, use
**only**:

```
fastboot stage dist/<artifact>.efi
fastboot oem boot-efi
```

RAM staging avoids flashing the EFI to a partition; the staged payload is
discarded on a power cycle. It does not make firmware execution generally
read-only: the boot chain may still update logs or boot metadata.

Device interaction requires explicit user coordination. If a non-HLOS
flash is genuinely needed, explain the proposed command and risks, and
let the user execute it in their own terminal. These are documented safety
constraints; do not assume a particular tool installs or enforces hooks.

## Workflow: local merges

Integrate changes into `main` locally, then push to the maintained remote.
A pull request is not required; open one only when explicitly requested.

- Small, focused changes may be committed directly to `main` when requested.
- Feature branches are useful for larger or experimental work. When ready
  and integration is requested, merge them into `main` locally and push.
- Keep commits focused. Version bumps (`VERSION` + `CHANGELOG.md`) stay
  separate from feature work, without a mandatory release PR.
- Never force-push `main`. Push a follow-up commit instead.

Archived upstream plans and specifications are historical records, not
current workflow requirements.

Follow explicit user requests about target branches and review workflow.
Do not commit, push, create branches, open PRs or tag releases unless
requested. Before a push, verify the destination remote and branch rather
than assuming `origin` is the intended repository.

The maintained repository is `Osilvfe/gbl-chainload`. In this checkout,
`osilvfe` points to that repository and `origin` points to the original
upstream. Verify these mappings before publishing; do not push to the
original upstream by accident.

## File locations

Firmware lives in `GblChainloadPkg/`, shared Rust libraries in `crates/`,
and the host/recovery multicall tool in `tools/gbl/`. The pinned `edk2/`
submodule supplies the firmware build core. Keep these conventional build
paths stable unless the task explicitly calls for restructuring them.

Use `docs/README.md` as the documentation map. Keep technical findings in
`docs/research/`, integration notes in `docs/integrations/`, and project
decisions in `docs/project/`. Dated plans, specifications and validation
records belong in `docs/archive/`, not hidden session-note directories.

Curated firmware inputs belong in `tests/fixtures/`; private OTA dumps and
scratch notes belong in ignored `local/`. Preserve existing `Build/`,
`dist/`, `target/` and device logs unless the task explicitly requires
changing them.

The `zip/` submodule currently supplies the recovery installer and its
prebuilt release inputs. A fresh CI build does not automatically replace
those inputs. Treat installer migration and submodule pointer updates as
explicit work, not incidental directory cleanup.

## Building

Builds use locally installed toolchains, not Docker. Cargo builds the Rust
tools and static libraries; the final EFI also requires EDK2 linking with
the AArch64 GNU toolchain. See the root READMEs for native dependencies.

```bash
git submodule update --init edk2
rustup target add aarch64-unknown-none
bash scripts/build.sh --debug --no-recovery
```

The unified entry produces `dist/gbl-chainload-debug.efi` and
`dist/host/gbl` with those flags; without `--debug` it defaults to release
logging. The standalone `scripts/build-native.sh` defaults to debug
logging and a `-gcc5.efi` suffix. Both use the EDK2 `RELEASE` target;
logging names alone do not identify optimization or toolchain behavior.

Recovery builds require a local Android NDK and the Rust target
`aarch64-linux-android`. Set `ANDROID_NDK_HOME` and check discovery with
`bash scripts/build-recovery-tools.sh --check-only` before a full build.
Do not clear all of `Build/` just to relink firmware: it also contains
UniXNU sources, components and prior validation artifacts.

## Validation

Start with checks covering the files changed, then expand coverage:

- Directory, guidance and fixture changes: `bash tests/host/102_repository_layout.sh`.
- Patch-engine changes: `cargo test --release --locked -p patch-engine --features host`
  and `bash tests/host/098_patch12_patch13_oem.sh`.
- Boot-flow, profile or QSEE changes: `bash tests/host/099_mode2_runtime.sh`.
- Build-script changes: `bash tests/host/100_native_build.sh`,
  `bash tests/host/101_build_defines.sh` and `bash tests/010_build_smoke.sh`.
- Full host-side suite: `bash tests/runall.sh` after its prerequisites are available.

Fetch real Git LFS recovery fixtures for tests needing their contents;
pointer files are not usable partition images. Some parity checks also
need reference outputs and a usable `avbtool.py`. Report missing inputs,
skipped checks and actual regressions separately; do not weaken assertions
or replace references merely to make a run pass.

When moving fixtures, preserve filenames and image bytes, update Git LFS
attributes, and verify that the indexed LFS pointers still reference the
same objects. Generated test outputs stay in ignored directories.

## Patch and artifact boundaries

- Patch11a/11b/11c are mode-2-only. Patch12/13 are Oplus-only. Preserve
  those gates and patch13's existing GPT write boundaries unless explicitly
  asked to change them.
- Cached ABLs already contain instruction rewrites. Runtime mode changes
  cannot undo old cached patches; regenerate from the correct stock inputs.
- Distinguish a base EFI from a device-specific packaged EFISP. Check the
  target ABL, profile/vbmeta input, OEM and manifest before packaging.
- Rebuild when source changes. Do not label an older artifact as the latest
  build; record toolchain, flags and input/output hashes when sharing one.
- Static patch matches and host tests are not proof of device bootability
  or data safety. Do not deliberately reproduce a black-brick scenario.
