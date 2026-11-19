# Device-side scripts

These scripts drive a **real phone**. They never flash anything: the only
device commands they issue are `fastboot getvar`, `fastboot oem usb-msc`,
`fastboot reboot` (failure cleanup only) and SCSI commands addressed to the
exported read-only LUN. Everything else is read-only host inspection.

The single exception is the opt-in `prepare --write`, which copies a generated
test file into an ESP volume; plain `prepare` only reads (`stat`, `sha256sum`)
and records what it found.

Nothing here runs automatically — `tests/runall.sh` only exercises them
offline through `tests/host/105_msc_selftest.sh`.

## `msc_selftest.sh`

End-to-end check of the experimental ISO/file USB MSC LUNs and the
`MODE SENSE` implementation.

```bash
# 1. Android, adb root: find a file that is already on a FAT volume and record
#    its path, size and SHA-256 in local/msc-selftest.env. Writes nothing.
tests/device/msc_selftest.sh prepare

# 2. Enter GBL Fastboot with the staged/installed EFISP that contains the
#    feature (hold Volume Up during the normal entry window)

# 3. Export, verify, eject, and confirm Fastboot comes back
tests/device/msc_selftest.sh run
```

`prepare` details:

- needs `adb` with root, because the ESP is only mounted (and only readable)
  while Android runs;
- read-only by default: it searches the mounted `vfat` volumes for
  `--from <path>` (default `/EFI/BOOT/BOOTAA64.EFI`), and records the
  volume-relative path, the size and the device's own SHA-256 in
  `local/msc-selftest.env` (ignored by git);
- `--mount /mnt/xxx` pins the volume instead of searching;
- a size that is not a multiple of 512/2048 also exercises the LUN's
  zero-padding path, so any real firmware file works;
- `--write` is the only mode that touches the device: it generates a
  deterministic 5 MiB + 1234 byte file and copies it to
  `<volume>/EFI/GBLMSC/selftest.bin` (plus a temporary `/sdcard` hop). Use it
  only when the volume has no usable file of its own.

`volumes` (read-only, Android side) lists the mounted vfat volumes and their
top-level entries, which is what you need when the default `--from` path does
not exist: GBL itself is loaded from `efisp`, a partition Android never
mounts, so a file is only usable if the firmware builds a
`SimpleFileSystem` for that same partition.

While `oem usb-msc` has a LUN exported the device has switched its USB role to
mass storage, so **the Fastboot interface is gone**: no `getvar`, no `oem`, no
version check. That is the normal state during an export, not an error.

`run` handles it: if it finds LUNs still exported it ejects them
(`START STOP UNIT`, LoEj=1), waits for `gbl-chainload_build` to answer, and
carries on. `eject` does the same thing on its own, and is a no-op on an idle
device. When neither a LUN nor a Fastboot interface exists, `run` prints
`fastboot devices` and `adb devices` so the two situations cannot be confused.

Fastboot quirks this script absorbs (and `tests/host/105_msc_selftest.sh`
pins, because each one cost a debugging session): the `fastboot` on this host
writes `getvar` results to **stderr**, so probes merge both streams; a GBL
build id is cached for the whole run instead of re-probed per step; and every
probe retries, since the interface can need a moment after an eject.

`run` cases (`--cases "file iso mixed"` by default):

| Case | Command | Verified |
| --- | --- | --- |
| `file` | `oem usb-msc file:<path>` | 512-byte blocks, RO, padded capacity, content hash, INQUIRY type 0, MODE SENSE header/block descriptor/pages, write rejected |
| `iso` | `oem usb-msc iso:<path>` | 2048-byte blocks, INQUIRY type 5 (CD-ROM), GET CONFIGURATION CD-ROM profile, READ TOC (one data track at LBA 0 plus the lead-out that carries the disc size), MODE SENSE page `0x2a` capabilities, GET EVENT STATUS NOTIFICATION (NEA, media class advertised), content hash, write rejected |
| `mixed` | `oem usb-msc boot_a,file:<path>` | two LUNs with independent block sizes and models |

Each case ejects through `START STOP UNIT (LoEj=1)` and waits for
`getvar gbl-chainload_build` to answer again. `--keep` stops after the export
so you can poke at the LUNs manually; `--dry-run` prints what it would do.

Reading the exported block devices needs root, so `run` asks `sudo` for
credentials once. A CD-ROM LUN (`iso:`) is bound by `sr_mod` and appears as
`/dev/sr*`; every other LUN appears as `/dev/sd*`.
A failed `oem usb-msc` (for example `media not found`) is reported as SKIP,
not as a failure, because a file the firmware cannot see is a placement
problem, not a protocol regression.

## `msc_probe.py`

Standalone SG_IO helper used by the self-test and handy on its own:

```bash
sudo python3 tests/device/msc_probe.py info      /dev/sdb
sudo python3 tests/device/msc_probe.py modesense /dev/sdb
sudo python3 tests/device/msc_probe.py mmc       /dev/sdb
sudo python3 tests/device/msc_probe.py eject     /dev/sdb
python3 tests/device/msc_probe.py self-test
```

It prints `key=value` lines, so `grep`/`awk` work without extra tooling.
`info` also reports whether the host device the probe is talking to is
`/dev/sg*`, `/dev/bsg/*` or the block node itself, which matters on kernels
that refuse SG_IO on the block device.
