# Experimental USB Mass Storage

GBL can temporarily replace its Fastboot USB interface with a USB Mass
Storage device exposing up to four LUNs. With no arguments it exposes a
freshly generated 1 MiB FAT12 RAM disk containing `README.TXT`. Normal builds
reject media arguments before storage access or USB handoff, and behave as a
**read-only** disk: every write is refused with DATA PROTECT. The
experimental backends - named partitions plus read-only ISO/file images -
stay behind `GBL_USB_MSC_PARTITION_EXPORT=1`; host coverage includes a
fairlady `boot_a` device test with repeated RAM/partition sessions and
Fastboot resume. Other devices and partition geometries remain unverified.

Write support is a further opt-in and off in every normal build. A
`GBL_USB_MSC_WRITE` build turns on the write path (WRITE(6/10/12/16),
SYNCHRONIZE CACHE and MODE SELECT); by default it accepts writes to the
in-memory RAM LUN only, where the "disk" is volatile RAM and discarded when
the session ends. Writing to a **real** partition needs still another flag,
`GBL_USB_MSC_WRITE_PARTITIONS`, plus an explicit `rw:<name>` LUN spec, and
still refuses any partition that starts before LBA 34 so the GPT itself can
never be overwritten. Without those flags no block write path exists in the
image at all. See [Write backend](#write-backend) below.

It never exports a whole UFS LUN, decrypts Android data, exports a previous
Fastboot download, or (in a plain build) writes to storage. The existing RAM
boot commands remain unchanged and unrelated to the MSC RAM LUN.

## Trying the implementation

Build a normal-policy base EFI, leaving the installed EFISP untouched:

```bash
bash scripts/build-native.sh --release --output dist/gbl-chainload-release-usb-msc.efi
```

This output is a **base EFI**, not a device-specific EFISP. Before staging,
package it with the device's verified stock ABL, OEM selection and mode-2
vbmeta/profile inputs using `scripts/efisp-package.py`, or retain the entire
validated GBLP1 overlay from that device's previously tested package. Verify
the resulting package with `gbl inspect`; do not substitute another device's
ABL or regenerate a profile from an unverified vbmeta.

Once already inside a GBL Fastboot supporting `oem boot-efi`, manually stage
the **complete device-specific package**, not the base EFI:

```bash
fastboot stage /path/to/device-specific-mode2-usb-msc.efi
fastboot oem boot-efi
```

Hold Volume Up during the normal three-second entry window to enter the
staged GBL Fastboot, then manually run:

```bash
fastboot oem usb-msc
```

Only a separately enabled experimental build accepts the following command;
it is not part of the normal-build device test procedure:

```bash
bash scripts/build.sh --experimental-usb-msc-partition-export --no-recovery --no-host
```

This produces `dist/gbl-chainload-msc-partitions.efi`, a **base EFI** that
still needs the same device-specific packaging described above. The native
entry also accepts the flag, together with `--release` or `--debug` and an
optional `--output`. Neither entry enables partition export by default.
The firmware's build identity gains `-msc-partitions`, visible through
`fastboot getvar gbl-chainload_build`; verify that identity before trying
the partition command in a user-coordinated session:

```bash
fastboot oem usb-msc boot_a
```

The name is exact and case-sensitive. `boot` does not select `boot_a` or
`boot_b`; there is no current-slot fallback. Up to four comma-separated
exact names may be exported as separate LUNs, for example
`fastboot oem usb-msc boot_a,boot_b`. Duplicate GPT labels are rejected
rather than choosing a UFS LUN.
Invalid names, whole-device handles, unsupported geometry, locked devices,
and failed preflight reads fail before switching away from Fastboot.
No partition label means the RAM test disk, never an implicit internal disk.
Each LUN keeps the partition's 512- or 4096-byte logical block size. The
implementation supports `REPORT LUNS`, 64-bit `READ CAPACITY(16)`, and
`VERIFY(10)` in addition to the read data path. A plain build rejects every
write with DATA PROTECT; a writable build only accepts one for a LUN opened
with the explicit `rw:` prefix (see [Write backend](#write-backend)), and a
bare name is still read-only.

### Read-only ISO and file LUNs

The same comma-separated list accepts files that live on an EFI system
partition, so an image can be inspected from another machine without
flashing it:

```bash
fastboot oem usb-msc iso:/EFI/ISO/test.iso
fastboot oem usb-msc file:/EFI/IMAGES/disk.img
fastboot oem usb-msc boot_a,iso:fs1:/EFI/ISO/test.iso
```

- `iso:<path>` reports peripheral device type 5 (CD-ROM) with 2048-byte
  logical blocks, so hosts treat it as an optical disc and can mount the
  ISO9660 filesystem directly. `GET CONFIGURATION` advertises only the
  CD-ROM profile. `READ TOC` reports one data track at LBA 0 followed by the
  lead-out entry (track `0xAA`) at the block just past the end of the image,
  in LBA or MSF form as the CDB asks. The lead-out is what `sr_mod` turns into
  the disc capacity (`cdrom_get_last_written`), so a zero or bogus lead-out
  makes the host address blocks past the image and log `LBA out of range`
  during its filesystem probing.
- `file:<path>` reports peripheral device type 0 (direct access) with
  512-byte logical blocks, for raw disk images.
- `fsN:` may follow the scheme to pin one volume; without it the volumes are
  searched in handle order and the first match wins. Paths are always
  volume-relative, never device paths, and `..` cannot escape the volume.
- The final logical block is zero padded when the file size is not a
  multiple of the block size, and reads are bounded by the padded size.
- Files are opened `EFI_FILE_MODE_READ` only. There is no write, rename or
  delete path, no extraction into RAM, and the file handle plus its 64 KiB
  bounce buffer are released when the session ends.
- Directories, empty files and unreadable files are rejected during
  preflight, before Fastboot hands the controller to MSC.

### RAM disk and writable partitions

The comma-separated LUN list understands three more forms:

- `ram:` (or no argument) - the in-memory LUN: a 1 MiB FAT12 image carrying
  `README.TXT`. `ram:<KiB>` sizes it explicitly (64 KiB to 64 MiB, a multiple
  of 512 bytes); a size other than 1 MiB starts blank. It is private to the
  session and discarded on eject, teardown or power loss, so nothing it holds
  can reach real storage. In a `GBL_USB_MSC_WRITE` build it is writable.
- `ro:<name>` - a named partition, explicitly read-only (identical to a bare
  `<name>`). Its serial begins `GBLRO-`.
- `rw:<name>` - a named partition opened for writing. Only a
  `GBL_USB_MSC_WRITE_PARTITIONS` build accepts it; any other build refuses the
  whole spec with `EFI_SECURITY_VIOLATION` rather than silently downgrading a
  name that says `rw` to read-only. Its serial begins `GBLRW-`, so a writable
  LUN is distinguishable before the first write. A partition whose
  `StartingLBA` is below 34 is refused with write access (see below).

A bare name remains read-only in a writable build; there is no implicit
writable export.

### MODE SENSE

`MODE SENSE(6)` and `MODE SENSE(10)` return the mode header with the
write-protected bit (set for a read-only LUN and clear for a writable one, so
Linux exposes the writable RAM/`rw:` LUN without a manual `blockdev --setrw`),
an 8-byte block descriptor with the LUN's density, block count and logical
block size, and the implemented pages:

- page `0x01` (read/write error recovery): a 12-byte page (page-length
  byte 10) with every recovery parameter off. The LUNs perform no automatic retry, correction or
  deferred error handling, so the page exists only so error-recovery-aware
  tools do not treat the LUN as SCSI-1;
- page `0x08` (caching): write cache disabled (`WCE=0`) and no read cache.
  `WCE` stays `0` on writable media too: an acknowledged write must already
  have reached the medium, so the host keeps write-through instead of
  logging a missing caching page and does not buffer writes that a power cut
  could lose;
- page `0x0a` (control): fixed-format sense data and no saved log
  parameters;
- page `0x1c` (informational exceptions control): a 10-byte page, every
  field zero, `TEST` off and no log. The medium cannot develop a predictive
  failure this device could report, so nothing is claimed;
- page `0x2a` (CD/DVD capabilities and mechanical status), optical LUNs
  only: a 20-byte page whose capability bits are all clear. A read-only
  data image cannot record CD-R/RW, play CD-DA, return C2 pointers, read
  DVD media or deliver sub-channel data, so claiming any of those would be
  false. The speed and mechanical-status fields read zero ("not available")
  rather than a guessed value. Answering ILLEGAL REQUEST here makes optical
  tools downgrade the LUN to a bare SCSI-1 drive;
- pages are emitted in ascending page-code order: `01,08,0a,1c` on every
  LUN, followed by `2a` on a CD-ROM LUN. Page code `0x3f` returns every page
  that applies to the LUN, and page code `0x00` returns only the header and
  block descriptor.

The DBD bit suppresses the block descriptor. Page control `0` (current) and
`2` (default) return the same bytes, `1` (changeable) and `3` (saved) are
rejected with ILLEGAL REQUEST `5/0x24`, as are unimplemented page codes,
including `0x2a` on a direct-access LUN. The response is truncated to the
allocation length.

### MMC event status

`GET EVENT STATUS NOTIFICATION` (opcode `0x4a`) is implemented for optical
LUNs. The image can never be ejected, replaced or written, so every poll
answers "no event available" (NEA) with an empty event descriptor, while the
supported-event-class mask still advertises the media class. Reporting a
media event instead would make the host drop its cached capacity and
re-read the disc on every check. On a direct-access LUN the opcode is
rejected with INVALID COMMAND OPERATION CODE `5/0x20`.

The normal build retains the existing policy: timeout starts Android and
Volume Up enters GBL Fastboot. Do not add `--auto` to this procedure: that
flag reverses these actions, so Volume Up attempts to chainload ABL instead.

`oem boot-efi` makes the staged bytes the preferred overlay source; a bare
EFI does **not** inherit the installed EFISP's cached ABL or mode-2 profile.
Without its own manifest, BootFlow defaults to mode 0 and may dynamically
patch the live ABL. That is not equivalent to the device's verified mode-2
boot chain. Although MSC itself does not need an ABL/profile, do not use
that fact to justify staging an unpackaged EFI for this device procedure.
If Orange State or a startup hang appears before `oem usb-msc`, stop MSC
testing and investigate the entry/chainload path first; do not flash or
change slots as part of this test.

The RAM test appears as a removable disk labelled `GBL RAM DISK`
(write-protected in a plain build, writable in a write build). Inspect
`README.TXT`. A real partition appears as a read-only
disk with the partition's original bytes and capacity, without a synthetic
partition table or filesystem. A raw boot image will not provide browsable
files, and encrypted userdata remains encrypted. Do not accept formatting
prompts or run repair tools; host filesystem support is independent of USB
enumeration. Inspect or back up the identified host device read-only, using
its reported size and identity rather than assuming a fixed `/dev/sdX` name.
Fastboot disappears while MSC owns the controller: this is intentional,
not a composite Fastboot + storage device. In a plain build host writes are
rejected with SCSI DATA PROTECT / WRITE PROTECTED, and neither the RAM disk
nor the exported partition has a write path. A writable build (see
[Write backend](#write-backend)) accepts writes only for the RAM LUN and,
with the extra opt-in, an `rw:` partition; a bare named partition and every
`file:`/`iso:` LUN stay read-only, and discarded OUT data for a refused write
never touches either medium. This does not suppress existing boot-chain
writes outside MSC and is not a promise that the exported bytes form a
snapshot.

Eject the disk using a host operation that sends SCSI START STOP UNIT,
press either volume key (or Escape), or disconnect the cable. GBL then
attempts to restore the same Fastboot session without a device reboot.
After reconnecting, verify it with `fastboot getvar gbl-chainload_build`.
Different host eject implementations need not send START STOP UNIT;
the keys remain the explicit local exit path.

## Write backend

Writing is a build-time opt-in, layered so a normal image has no write path
at all:

| Build | Flag | Writable LUN |
| --- | --- | --- |
| normal | (none) | none - every write refused `7/0x27` |
| RAM write | `GBL_USB_MSC_WRITE=1` | `ram:` only |
| partition write | `GBL_USB_MSC_WRITE_PARTITIONS=1` | `ram:` and `rw:<name>` |

The native and unified build entries expose these as
`--experimental-usb-msc-write` and `--experimental-usb-msc-write-partitions`
(both imply `--experimental-usb-msc-partition-export`). In a plain build the
`PartitionWrite`/`PartitionFlush` code and the block-export write helpers are
compiled out entirely, so there is no code path that can reach storage even
by mistake. Build identity carries `-msc-write[-partitions]` so
`fastboot getvar gbl-chainload_build` shows which policy is staged.

Commands:

- `WRITE(6/10/12/16)` is range-checked first (`5/0x21` if the block span
  falls outside the medium), then refused with DATA PROTECT `7/0x27` if the
  LUN is not writable. A refusal still keeps the declared OUT length so the
  bulk data stage drains cleanly and the next command starts in sync.
- `SYNCHRONIZE CACHE(10)` (`0x35`) calls the medium's flush. The RAM backend
  has no cache to commit; a partition backend forwards to
  `EFI_BLOCK_IO_PROTOCOL.FlushBlocks`.
- `MODE SELECT(6/10)` (`0x15`/`0x55`) accepts a parameter list that only
  re-states current values: the list is buffered whole and validated when
  the data stage completes. An unknown page or `WCE=1` is rejected with
  `5/0x26` (invalid field in parameter list); an unparsable or too-short list
  with `5/0x1a`; a list larger than 96 bytes is refused before the data stage.

Ordering and durability guarantees, which is what a host expects from a disk:

- **Every acknowledged write is visible to the next read.** RAM writes land
  in the same buffer reads come from, in command order. There is no
  write-back cache: the caching page keeps `WCE=0`, so the host does not
  buffer writes it thinks the device will commit later, and a write that has
  returned is already on the medium.
- **Partial blocks never tear.** A write that does not start and end on a
  partition-block boundary is done read-modify-write: the affected physical
  block is read, the changed bytes are overlaid, and the whole block is
  written back once. A power cut can lose the update, but it cannot leave
  half of one block new and half old.
- **Flush points.** A dirty writable LUN is flushed at SYNCHRONIZE CACHE, on
  eject (START STOP UNIT with LoEj), and when the session tears down, so an
  orderly exit commits everything. An abrupt power loss may lose the last
  un-synced writes, which is exactly the guarantee the caching page
  advertises.
- **The GPT is out of reach.** A writable partition must report
  `StartingLBA >= 34`; the protective MBR, the GPT header and the entry
  array live in LBAs 0..33, so the export refuses to hand any of them out
  writable even if such a partition existed.
- **Concurrency.** The BOT layer is strictly one command at a time: a write
  completes (data stage drained, CSW sent) before the next CDB is parsed, so
  there is no interleaving to reason about. The medium is private to the
  session; no other firmware component reads it while MSC owns the
  controller.

The RAM LUN is the place to exercise this without touching real storage:
`ram:` in a writable build accepts writes, shows them on the next read,
honours SYNCHRONIZE CACHE, and is thrown away on eject. The device self-test
uses exactly that - see below.

## Device-validation sequence

The 2026-11-18 fairlady test covered exact-name `boot_a` enumeration,
capacity and write protection, plus repeated RAM/partition switching and
Fastboot resume. It did not complete the reference hash comparison below;
use the full sequence for new devices or partition geometries.

1. Keep the installed, working EFISP and its slot selection unchanged.
   Package the experimental base with the complete overlay of the exact
   device package already confirmed to boot; preserve its cached ABL,
   profile and manifest bytes. Do not infer the right profile from a filename.
2. Manually stage that complete package and enter GBL Fastboot. First run
   the RAM disk without a partition argument, inspect `README.TXT`, exit,
   and confirm that the same Fastboot build responds again. This is still
   needed even though initial RAM-disk enumeration was observed earlier.
3. Only then export one explicitly named partition, initially `boot_a`.
   Check the new host device's USB identity, serial beginning `GBLRO-`,
   reported capacity and read-only flag. Do not assume a fixed `/dev/sdX`,
   try to mount a raw boot image, or accept a formatting prompt.
4. Read its bytes into a regular host file and compare size and SHA-256
   with a full-partition read obtained read-only from Android or recovery.
   A shorter flashable image is not an equivalent reference unless the
   comparison is explicitly restricted to the same byte range. Do not
   send write commands to a real partition to test write rejection.
5. Eject or use a volume key to return to Fastboot, confirm a command works,
   then repeat the RAM-disk/partition transition without a device reboot.
   Stop on a missing device, transfer error or failure to resume Fastboot.

These steps require explicit user coordination. Host lifecycle simulations
do not establish that the Qualcomm controller actually resumes Fastboot on
the device, and this work does not attempt writable MSC or USB Host mode.

RAM staging and this MSC backend do not make the entire firmware boot
chain read-only. Existing early initialization, logging and boot metadata
policies still apply. Do not flash EFISP just to try this feature.

## Implementation boundaries

- `UsbMscScsi.c` generates the FAT image and implements bounded SCSI
  processing plus CBW/CSW validation and BOT direction/residue rules.
  Capacity and byte offsets are 64-bit; READ CAPACITY(16) and READ(16) cover
  partitions beyond READ CAPACITY(10)'s address range. Each LUN reports its
  backend's 512- or 4096-byte logical sector size; READ(6/10/12/16),
  WRITE(6/10/12/16) (range-checked, then accepted only on a writable LUN),
  REPORT LUNS, VERIFY(10) and MODE SELECT are bounded by that size. Multiple LUNs need both BOT class paths to agree: GET MAX LUN returns
  the highest LUN index, and each REPORT LUNS entry uses SCSI peripheral device
  addressing (bus in byte 0, LUN in byte 1). A list that puts the LUN in the
  leading byte is decoded by hosts as a different LUN number, so the export
  then enumerates only LUN 0. INQUIRY reports SPC-3 (version `0x05`); hosts gate
  VPD page 0x80/0x83, REPORT LUNS and multi-LUN probing on that level, so an
  SPC-2 answer makes Linux fall back to a sequential LUN scan and never ask for
  the per-LUN serial number page.
- `UsbMscMedia.c` provides the RAM and partition backends. A named export
  requires a unique GPT record and a non-removable logical-partition BlockIO
  handle with matching partition-relative geometry. No whole-disk or active
  slot lookup is used. Reads use a fixed 64 KiB aligned bounce buffer to
  translate logical-block requests to physical blocks of 512 to 65536 bytes,
  including unaligned reads on 4 KiB UFS media. Media ID, size, block size
  and alignment changes fail reads instead of silently switching media.
  Read errors return SCSI sense/failed status, preserving the Fastboot return
  path rather than treating a storage error as a fatal USB session error.
  The RAM backend owns a variable-size buffer (`ram:[KiB]`); the partition
  backend's write/read-modify-write/flush helpers are compiled only under
  `GBL_USB_MSC_WRITE_PARTITIONS`, so a plain image has no block write path.
- `UsbMscUsb.c` owns the lower-level Qualcomm `EFI_USBFN_IO_PROTOCOL`
  session, descriptors, endpoint-zero requests, bulk transfers and reset
  recovery. It requires protocol revision `0x00010003` or later. It routes
  WRITE and MODE SELECT OUT data stages into the BOT layer and flushes dirty
  LUNs at teardown. USB full/high/super-speed descriptors are provided;
  actual controller and host compatibility still require hardware testing.
- `UsbMscFastboot.c` registers
  `oem usb-msc [exact-partition-name[,exact-partition-name...]]`, rejects
  extra arguments and an in-flight flash, checks the protocol and partition,
  and waits at most five seconds for the Fastboot acknowledgement before
  requesting the handoff. Cancelled or timed-out handoffs release the
  prepared partition backend without stopping Fastboot.
- `scripts/edk2/gbl-usb-msc.patch` adds a `GBL_USB_MSC`-gated callback to
  FastbootLib. The native build applies it reproducibly without changing
  the pinned EDK2 commit. The switch runs outside menu timer callbacks;
  command registrations, download buffers and watchdog handling are kept.

MSC and Fastboot never intentionally own the USB controller simultaneously.
For endpoint zero, only an explicit rejection requests STALL, once on the
OUT direction. A new SETUP packet lets the controller recover the control
pipe; the class driver must not unconditionally call
`SetEndpointStallState(EP0, ..., FALSE)`. Qualcomm's DWC3 implementation
routes every EP0 stall-state call, including `FALSE`, to its control STALL
state machine. Calling it before replying to a descriptor request therefore
prevents enumeration. Non-control endpoint halt/clear handling is separate.

Reset/configuration waits for aborted DMA transfers to finish before
reusing their buffers. Invalid CBWs require class reset followed by clearing
both bulk endpoint halts; ordinary clear-halt alone cannot resume them.
A two-second cancellation deadline fails the MSC session rather than
waiting forever. DMA allocations and descriptor backing storage are retained
if stopping the controller fails, and Fastboot is not restarted on top of a
possibly active controller. Such a failure can require a manual power cycle.

The prototype reuses the VID/PID supplied by the device firmware and uses
a distinct MSC serial string. It does not allocate a new product ID;
host driver binding, particularly Windows' cached Fastboot bindings, remains
part of hardware validation. No USB Host or UAS support is added.

A refused data-in command answers with a **short data stage followed by CSW
status 1** (CHECK CONDITION), not with a stalled IN endpoint: hosts turn a
stalled data phase into bulk-only reset recovery, which hides the sense data
(Linux then reports a bare I/O error instead of `ILLEGAL REQUEST`). The short
packet is deliberately *not* a zero-length transfer: the firmware ends such a
data stage with `min(host length, bulk packet - 1)` bytes, a single non-full
packet at every supported speed, so the host's transfer always ends without
depending on the controller turning a zero-length URB into a ZLP. The bytes
carry no meaning; the CSW status is authoritative, and the CSW residue counts
them as transferred so hosts do not see a phantom shortfall. Verified on the
device: `mode6_pc1` comes back as `CHECK CONDITION 5/0x24` with the framed data
stage in the buffer and `dCSWDataResidue` of zero. The same rule covers `MODE SENSE`
page-control/page refusals, reads past the end of the medium, and any answer
truncated to zero bytes by the allocation length.

## Automated device self-test

`tests/device/msc_selftest.sh` wraps the whole procedure. The Android phase
(`prepare`) is read-only: it locates a file that is already on a mounted FAT
volume (by default `/EFI/BOOT/BOOTAA64.EFI`) and records its path, size and
SHA-256 in `local/msc-selftest.env`; nothing is created on the device. Only
`prepare --write` generates a deterministic 5 MiB + 1234 byte file and copies
it to `EFI/GBLMSC/selftest.bin`, for devices where no usable file exists yet.

The GBL Fastboot phase (`run`) exports that file as `file:` and `iso:`,
verifies logical block size, read-only flag, padded capacity, content hash,
INQUIRY type, `MODE SENSE` header/block descriptor/pages (the
`01,08,0a,1c[,2a]` set with the fixed 12- and 10-byte pages `0x01` and `0x1c`
(their page-length bytes 10 and 8), `0x2a` present on the CD-ROM LUN and
refused on a direct-access one),
`MODE SELECT` (a current-value list accepted, `WCE=1` and an unknown page
refused `5/0x26`), and the MMC commands with their event-status answer, tries
a write on each read-only LUN, and ejects back to Fastboot. A `mixed` case
exports `boot_a` together with the file LUN, and the reference hash comes from
the device's own `sha256sum` of the recorded file.

A `ram` case exports the in-memory LUN and drives the write backend: a
read-only build must refuse `WRITE(6/10/12/16)` with DATA PROTECT and change
nothing; a writable build must accept all four CDB sizes, show a 2-block
round-trip on the next read, survive SYNCHRONIZE CACHE, and restore the medium
exactly as found. A `rw` case is the only one that writes real storage: it is
opt-in (listed only in `--cases` and gated on `MSC_RW_PARTITION=<name>`) and it
writes and restores just the **last block** of `rw:<name>`, skipping itself
unless the LUN reports `WP=0`. See
[tests/device/README.md](../../tests/device/README.md) for the options; the
offline replay of the script itself is `tests/host/105_msc_selftest.sh`.

## Host validation

```bash
bash tests/host/103_usb_msc.sh
```

The harness runs the production SCSI, USB event and Fastboot command code
with AddressSanitizer and UndefinedBehaviorSanitizer. Coverage includes all
13 BOT host/device direction-length cases, 20,000 malformed/random CBWs,
unchanged disk contents after rejected writes, descriptor/control requests,
full enumeration and recovery from an unsupported control request with a
Qualcomm-style EP0 stall-state mock, exact partition-name selection, locked
device/whole-disk/duplicate-label rejection, 4 KiB alignment and partial reads,
large-media READ CAPACITY(16)/READ(16), storage read/media-change errors,
partition preservation across USB reset, write rejection and handoff cleanup,
delayed cancellation, reset recovery, eject completion, allocation failures,
failed controller shutdown, and bounded Fastboot ACK waiting. `fsck.fat -n`
also checks the generated image when installed. A clean source copy checks
that the Fastboot integration patch applies and reverses.
The harness also alternates RAM and partition media over 15 MSC sessions,
covering eject, an initial detach followed by attach/detach, Volume Up,
Volume Down and Escape exits while checking DMA and media cleanup. Both
the default command guard and opt-in command path are compiled and tested.
Additional cases cover four-LUN REPORT LUNS, LUN selection, invalid LUN
sense, 4 KiB logical blocks, READ CAPACITY 16, VERIFY(10), and deterministic
write rejection. Mode-sense coverage checks both CDB sizes, the block
descriptor, the DBD bit, the page set `01`/`08`/`0a`/`1c`/`2a`/`0x3f`/`0x00`,
the rejected page controls and page codes, and allocation-length truncation.
The harness builds four firmware variants - `0:0:0`, `1:0:0`, `1:1:0` and
`1:1:1` for partition-export/write/write-partitions - and a text check
confirms the block write/flush helpers exist only inside
`GBL_USB_MSC_WRITE_PARTITIONS` regions and that the RAM region has no BlockIO.
The write backend gets direct coverage too: RAM sizing and bounds, a refused
write leaving the content unchanged, write-then-read ordering, SYNCHRONIZE
CACHE clearing dirty state, range refusal, MODE SELECT accept/`WCE`/unknown/
overlong lists, `rw:` partition write with read-modify-write, a flush on
close, the `LBA < 34` refusal, the `ro:`/`rw:` spellings, and `rw:` being
refused in a non-write build. File media
coverage drives a mocked `EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` and
`EFI_FILE_PROTOCOL`: ISO/file block sizes, CD-ROM INQUIRY, zero padding of a
short final block, `fsN:` selection, path normalisation, directory/empty
file rejection, and a read error propagating as CHECK CONDITION.

These are simulated protocol and build checks, not proof that a particular
phone enumerates MSC correctly. No device commands run as part of the test.

On the 2026-11-19 fairlady test, direct SG_IO from the host confirmed the
device replies correctly to `REPORT LUNS` (LUN list of `00 01 …` entries),
`INQUIRY` page 0x00 and page 0x80 per LUN. The same host kernel
(Linux 7.3.4-zen) nevertheless never issued an EVPD INQUIRY or a REPORT LUNS
during `usb-storage` probing and did not cache page 0x80, even after
`device/rescan`, so `lsblk SERIAL` showed the single USB string instead of the
per-LUN serial and its scan log reported a sequential LUN scan. Treat that as a
host quirk: verify identity with `REPORT LUNS`/SG_IO or the `HCTL` field, not
with `lsblk SERIAL`. Writes are stopped before the device on such hosts:
opening an exported LUN with write access fails with `EROFS`.

## Enumeration failure observed on fairlady

On 2026-11-18 the Linux host reported `device descriptor read/all, error -32`,
then descriptor timeouts (`-110`) and `unable to enumerate USB device`
immediately after switching away from Fastboot. No MSC interface or block
device was created, so filesystem mounting was not the failing stage.

The original implementation cleared both EP0 halt directions before every
SETUP response. The behavior described above is present in
[`UsbfnDwcSetEndpointStallState` in Qualcomm's published source](https://github.com/Rivko/android-firmware-qti-sdm670/blob/20bb8ae36c93fc16bbadda0e0a83f930c0c8a271/boot_images/QcomPkg/Drivers/UsbfnDwc3Dxe/UsbfnDwc3Impl.c#L1792)
and was also checked in the local fairlady `UsbfnDwc3Dxe.efi` sample
(SHA-256 `818dc4ae0b88ed78d4693980aee02a15607f3465485f7dc17344de7afe5b150b`).
The regression mock reproduces the failure on the first descriptor request
before the fix; removing those EP0 clear calls and using one OUT-direction
STALL for rejected requests passes the enumeration and recovery tests.
After the EP0 fix, the host detected the RAM test on 2026-11-18 at 12:43
(Asia/Shanghai): `usb-storage`, 480 Mbps, 2048 sectors of 512 bytes, FAT and
label `GBL RAMDISK`, with `Write Protect is on`. That established RAM-disk
enumeration and host metadata reads. On the same fairlady device, a
2026-11-18 coordinated test exported `boot_a` as a 96 MiB write-protected
disk, confirmed it was the boot image, and repeatedly switched between RAM
and partition media with Fastboot resume. This does not establish write
rejection for every host, every partition geometry, or other devices; those
cases still need coordinated stage tests.

## Write-path validation on fairlady

On 2026-11-20 the `msc-rev10-write` EFISP (INQUIRY build marker `0010`) was
staged with `fastboot stage` + `oem boot-efi` and exercised with
`tests/device/msc_selftest.sh`. Two results matter:

- `run --cases ram`: the writable RAM LUN enumerated as 2048×512 with
  `Write Protect is off` (MODE SENSE `3f 00 00 08`), and the write probe
  accepted WRITE(6/10/12/16), showed a 2-block round-trip on the next read,
  survived SYNCHRONIZE CACHE, and restored the medium — 23/23 checks. The
  earlier failure was a hardcoded WP bit in `MODE SENSE`, which made Linux
  mount even a writable LUN read-only; `ModeSense` now reports WP from
  `Bot->Writable`.
- `run --cases rw` with `MSC_RW_PARTITION=logfs`: the exported 8 MiB
  `logfs` partition (2048×4096) reported `WP=0`, and the probe wrote and
  restored only its last block, verifying the read-back, barrier and restore
  — 12/12 checks, back in GBL Fastboot afterwards. `logfs` is the preferred
  target because it only holds the boot trace (`GblTrace.txt`,
  `GblChainload_BootN.txt`).

Both runs cover one host and one fairlady unit; other partition geometries and
hosts still need their own coordinated runs.
