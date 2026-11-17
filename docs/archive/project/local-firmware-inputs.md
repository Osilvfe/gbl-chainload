# Historical firmware-input notes

> Earlier per-device input conventions, retained for provenance. The current
> private-workspace policy is documented in [local/README.md](../../../local/README.md).

Gitignored by default. Populated by the developer with stock OTA partition
images for fixture extraction.

## Expected inputs

- `local/firmware/canoe-stock-A.07_2024_02_05/abl_a.bin` — stock A.07_2024_02_05 ABL.
  Manual prerequisite — extract from a stock OTA when available.
- `local/firmware/infiniti/LinuxLoader_infiniti.efi` — symlink-ok to
  `/home/vivy/gbl_root_canoe/images/LinuxLoader_infiniti.efi`.
- `local/firmware/infiniti-EU-16.0.5.703/abl.bin` — extracted from EU 16.0.5.703 OTA
  (`abl.img` renamed to `abl.bin`; note: partition is named `abl`, not `abl_a`).
- (Plan 2) `local/firmware/canoe-stock-A.07_2024_02_05/{boot,recovery,dtbo,vbmeta}.img`
  for mode-2 profile extraction.

## Outputs

`scripts/extract-canoe-fixtures.sh` produces `local/firmware/fixtures/canoe-A.07/abl_a.bin`
which `tests/042_dynamic_patch_harness.sh` picks up.

## infiniti-EU-16.0.5.703 fixture notes

The EU 16.0.5.703 OTA contains raw partition images (not payload.bin).
The ABL partition is named `abl.img` (not `abl_a.img`) — this is the A-slot
image. It was copied to `local/firmware/infiniti-EU-16.0.5.703/abl.bin` at task-15
setup time. SHA256 is recorded in `local/firmware/infiniti-EU-16.0.5.703/sha256sums.txt`.

## canoe-A.07 fixture notes

No stock canoe A.07 OTA is yet available. This is a manual prerequisite.
When obtained, drop `abl_a.bin` or `abl_a.img` into
`local/firmware/canoe-stock-A.07_2024_02_05/` and run
`scripts/extract-canoe-fixtures.sh` to populate the fixture.

CI (`tests/042`) runs anchor uniqueness checks against every available fixture.
Absent canoe fixture means CI covers only gbl_root_canoe infiniti + EU 16.0.5.703.
