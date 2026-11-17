# Local workspace / 本地工作区

Everything under this directory except this README is ignored by Git.
Use `local/firmware/` for private OTA dumps and extracted partition images,
and `local/notes/` for temporary investigation data. Absolute paths outside
the repository are also valid inputs to the packaging tools.

这里存放本地固件输入和临时研究数据，不进入版本控制。不要将设备日志、
分区备份或待测试 EFI 当作公开测试样本提交。

- Reviewed regression samples belong in [tests/fixtures/](../tests/fixtures/README.md).
- Fixture extraction helpers use `local/firmware/` as their local workspace.
- Build caches and outputs remain in `Build/`, `target/` and `dist/`.
- Device log helpers still write to the existing ignored `logs/` directory.

The former root-level firmware input directory has been replaced by
`local/firmware/`. No private firmware files are moved automatically.
