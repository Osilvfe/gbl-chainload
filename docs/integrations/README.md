# Optional integrations

- [UniXNU on the CLO r25 core](unixnu-clo-r25.md): source pins, shared EDK2
  build, embedded EFI components and read-only RAM FAT payload loading.
- [USB Mass Storage](usb-mass-storage.md): read-only RAM disk, USB controller
  handoff and host-side validation; named-partition export is disabled by default.
- [Fastboot slot attributes](fastboot-slot-attributes.md): inspect and repair
  `boot_a` / `boot_b` GPT attribute words.

These components are optional. Normal startup continues to target Android.
