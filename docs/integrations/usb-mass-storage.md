# Experimental USB Mass Storage

GBL can temporarily replace its Fastboot USB interface with a single-LUN,
read-only USB Mass Storage device. With no arguments it exposes a freshly
generated 1 MiB FAT12 RAM disk containing `README.TXT`. Normal builds reject
partition arguments before storage access or USB handoff. The named-partition
backend remains experimental, behind `GBL_USB_MSC_PARTITION_EXPORT=1`; host
coverage includes a fairlady `boot_a` device test with repeated
RAM/partition sessions and Fastboot resume. Other devices and partition
geometries remain unverified. It never exports a whole UFS LUN,
writes to storage, decrypts Android
data, or exports a previous Fastboot download. The existing RAM boot commands
remain unchanged; a writable RAM payload-transfer disk is not part of MSC.

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
`boot_b`; there is no current-slot fallback. Only one partition is exported,
and duplicate GPT labels are rejected rather than choosing a UFS LUN.
Invalid names, whole-device handles, unsupported geometry, locked devices,
and failed preflight reads fail before switching away from Fastboot.
No partition label means the RAM test disk, never an implicit internal disk.

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

The RAM test appears as a removable, write-protected disk labelled
`GBL RAMDISK`. Inspect `README.TXT`. A real partition appears as a read-only
disk with the partition's original bytes and capacity, without a synthetic
partition table or filesystem. A raw boot image will not provide browsable
files, and encrypted userdata remains encrypted. Do not accept formatting
prompts or run repair tools; host filesystem support is independent of USB
enumeration. Inspect or back up the identified host device read-only, using
its reported size and identity rather than assuming a fixed `/dev/sdX` name.
Fastboot disappears while MSC owns the controller: this is intentional,
not a composite Fastboot + storage device. Host writes are rejected with
SCSI DATA PROTECT / WRITE PROTECTED; discarded OUT data never touches either
the RAM disk or the exported partition. MSC does not call BlockIO write,
flush or reset methods. This does not suppress existing boot-chain writes
outside MSC and is not a promise that the exported bytes form a snapshot.

Eject the disk using a host operation that sends SCSI START STOP UNIT,
press either volume key (or Escape), or disconnect the cable. GBL then
attempts to restore the same Fastboot session without a device reboot.
After reconnecting, verify it with `fastboot getvar gbl-chainload_build`.
Different host eject implementations need not send START STOP UNIT;
the keys remain the explicit local exit path.

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

- `UsbMscScsi.c` generates the FAT image and implements bounded, read-only
  SCSI processing plus CBW/CSW validation and BOT direction/residue rules.
  Capacity and byte offsets are 64-bit; READ CAPACITY(16) and READ(16) cover
  partitions beyond READ CAPACITY(10)'s address range. The USB-facing logical
  sector size remains 512 bytes.
- `UsbMscMedia.c` provides the RAM and partition read backends. A named export
  requires a unique GPT record and a non-removable logical-partition BlockIO
  handle with matching partition-relative geometry. No whole-disk or active
  slot lookup is used. Reads use a fixed 64 KiB aligned bounce buffer to
  translate 512-byte USB requests to physical blocks of 512 to 65536 bytes,
  including unaligned reads on 4 KiB UFS media. Media ID, size, block size
  and alignment changes fail reads instead of silently switching media.
  Read errors return SCSI sense/failed status, preserving the Fastboot return
  path rather than treating a storage error as a fatal USB session error.
- `UsbMscUsb.c` owns the lower-level Qualcomm `EFI_USBFN_IO_PROTOCOL`
  session, descriptors, endpoint-zero requests, bulk transfers and reset
  recovery. It requires protocol revision `0x00010003` or later. USB
  full/high/super-speed descriptors are provided; actual controller and
  host compatibility still require hardware testing.
- `UsbMscFastboot.c` registers `oem usb-msc [exact-partition-name]`, rejects
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

These are simulated protocol and build checks, not proof that a particular
phone enumerates MSC correctly. No device commands run as part of the test.

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
