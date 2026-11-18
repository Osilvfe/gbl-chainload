# Fastboot slot attribute commands

These are experimental diagnostic and repair commands, not a guarantee of
bootability or data safety. `slot-show` is read-only; `slot-set` changes
persistent boot metadata only when explicitly invoked without `dry-run`.
Do not deliberately exhaust retries or mark both slots unbootable to test it.

The GBL Fastboot build registers two OEM commands for inspecting and repairing
the A/B slot metadata stored in the `boot_a` / `boot_b` GPT entries:

```text
fastboot oem slot-show [a|b]
fastboot oem slot-set <a|b> [active=N] [priority=N] [retry=N] \
    [successful=N] [unbootable=N] [dry-run]
```

`slot-show` reads both GPT copies from storage and checks their CRCs and
agreement. It prints diagnostic `INFO` responses, waiting for each USB transfer
to complete before sending the next response. This is required because common
Fastboot clients discard the text carried in a terminal `OKAY` response.
With no suffix, it prints both slots:

```text
GPT primary/backup: crc=ok, entries=identical
UFS boot-lun=1 (a=1 b=2)
a:007f000000000000 act=1 pri=3 ret=7 succ=1 unb=0
b:0000000000000000 act=0 pri=0 ret=0 succ=0 unb=0
```

## What it changes

Both slots include the full 64-bit attribute word and decoded fields.
`slot-show a` or `slot-show b` limits the slot lines to the selected slot.
The boot LUN is read directly through the card-info protocol, and underlying
read errors are reported instead of silently treating the output as valid.

The command also prints a GPT GUID snapshot for `boot_a`, `boot_b`, `abl_a`
and `abl_b`: each entry's LUN, raw attributes, type GUID (`type`), unique
partition GUID (`uuid`), and start/end LBAs. GUIDs come from on-disk GPT
entries, not the cached partition records. If ABL lives on another LUN, that
LUN's primary and backup GPTs are read and validated too. Missing or ambiguous
names, invalid LUNs, or invalid GPT copies make the diagnostic fail explicitly.
The optional slot suffix filters the displayed entries to that slot.

The final `standard boot type GUID` line reports where the known Qualcomm
boot type `20117f86-e985-4357-b9ee-374bc1d8487d` appears: `a`, `b`, `both`,
or `neither`. The `boot/abl type GUIDs` lines report whether those two
partition types are the same within each slot. They help diagnose the GUID
mapping when the active flags and UFS boot LUN disagree. They do not verify
partition image contents or establish that a slot is safe to boot.

Qualcomm's normal slot-switch operation swaps **partition type GUIDs** across
slot pairs. It does not swap the unique partition UUIDs. The diagnostic prints
both to avoid confusing those two GPT fields, and performs no GUID writes or
implicit metadata repair.

Only the requested GPT attribute bits of the specified `boot_a` or `boot_b`
entry are edited. The command preserves other entries, GUIDs, unrelated
attribute bits and block padding. It validates both GPT copies before writing,
then writes the affected entry block and header in the backup GPT first,
followed by the primary GPT. Each copy is flushed and compared with the expected
bytes read back from storage. The peer slot is never changed implicitly.

If a write, flush or read-back check fails after writing starts, the command
attempts to restore the original blocks and verifies them. A failed rollback is
reported explicitly. These writes are not power-loss atomic; a successful host
test does not establish device bootability or data safety.

Supported fields:

| Field | Short alias | Range | GPT bits |
| --- | --- | --- | --- |
| `active` | `act` | 0..1 | 50 |
| `priority` | `pri` | 0..3 | 48..49 |
| `retry` | `ret` | 0..7 | 51..53 |
| `successful` | `succ` | 0..1 | 54 |
| `unbootable` | `unb` | 0..1 | 55 |

Use short aliases when specifying several fields: Fastboot commands have a
64-byte limit. Duplicate fields, including a long name and its alias, are
rejected. Values are single decimal digits within the listed range.
`dry-run` performs the validation and prints the proposed attribute change
without modifying GPT or the bootloader's partition cache.

## What it does not do

`slot-set` deliberately does **not** perform the full `fastboot set_active`
flow. It does not swap partition GUIDs and does not change the UFS boot LUN.
Use it to repair slot metadata. When setting `active=1` on UFS, the selected
slot must agree with the existing boot LUN (`a` = 1, `b` = 2). This prevents an
attribute-only edit from silently creating a slot/LUN mismatch. A complete
slot switch still requires the platform's `set_active` implementation.

The command refuses the obvious dangerous states:

- two active slots;
- two unbootable slots;
- active + unbootable on the same slot;
- active with priority 0;
- active with `successful=0` and `retry=0`;
- `unbootable=1` together with `successful=1`;
- clearing the last active slot.

It also requires an unlocked device, an idle flash operation and no snapshot
merge in progress. `snapshotted` is permitted; the command does not change
snapshot metadata. Both boot partitions must be on the same LUN. Invalid CRCs,
disagreeing GPT copies, ambiguous partition names or storage handles, changed
media, and unsupported GPT layouts are rejected before writing.

Each failed attribute check reports a specific reason. For example,
`boot-lun=2; target=a requires=1` means the attribute-only activation of A
would conflict with the UFS boot LUN. `peer boot_b is active` identifies a
different refusal: creating two active slots. Do not remove the guards or infer
a safe target from a generic access-denied response.

Use `slot-show` for immediate read-back. Ordinary `getvar` slot responses can
retain values published when Fastboot started. After a successful write the
BootLib partition cache and partition-record attributes are updated, but the
command does not refresh the previously published Fastboot variables.

## Recovery example

For a missing-active-slot condition, with UFS boot LUN 1 and a known valid slot
A, the following command previews the proposed repair:

```text
fastboot oem slot-show
fastboot oem slot-set a act=1 pri=3 ret=7 succ=1 unb=0 dry-run
```

After reviewing the output, removing `dry-run` performs the persistent GPT
write. Read it back with `fastboot oem slot-show`. Do not infer which slot is
safe to boot from its attributes alone; firmware contents and OTA snapshot
state also matter. Staging this EFI only registers the commands; it does not
run `slot-set` automatically.

Host validation: `bash tests/host/104_slot_attrs.sh` exercises the actual
command handlers against simulated 512-byte and 4096-byte GPT storage, with
ASan/UBSan and injected write, flush, read, allocation and media-change errors.
It also covers GUID formatting, slot filtering, all four standard-boot-GUID
mapping results, separately validated ABL LUNs, and reporting on-disk GUIDs
when cached ABL GUIDs differ.

## Device validation and unresolved GPT warning

On fairlady, live diagnostics and an explicit `boot_b` attribute edit were
exercised on 2026-11-18. Read-back reported matching primary/backup entries
and valid CRCs. The subsequent Android boot failed in first-stage init's
AVB verification, and its kernel log also reported `Alternate GPT is invalid,
using primary GPT.` The timing and cause of that GPT warning are not yet
established; neither the successful read-back nor the AVB diagnosis proves
that all later firmware writes and Linux GPT checks are compatible.

The host regression reproduces the observed missing-active-slot state and
checks that only the requested `boot_b` attributes and GPT CRCs change,
preserving A, ABL entries, GUIDs and boot LUN. This is not a device-wide
validation of GPT repair or power-loss recovery. Preserve backups and do
not bypass a GPT validation failure or treat a successful write as proof
that the target slot is safe to boot.
