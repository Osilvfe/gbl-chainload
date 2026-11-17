# Documentation / 文档导航

The root [English README](../README.md) and [中文 README](../README.zh-CN.md)
describe the current build, packaging and device-testing interfaces.

Repository-wide contribution and device-safety rules live in
[AGENTS.md](../AGENTS.md), independently of the editor or assistant used.

| Directory | Contents / 内容 |
| --- | --- |
| [research/](research/README.md) | Reverse-engineering findings, AVB references and investigations / 逆向结论与研究 |
| [integrations/](integrations/README.md) | Optional component integration / 可选组件集成 |
| [project/](project/README.md) | Project decisions and upstream issue tracking / 项目决策与上游问题 |
| [archive/](archive/README.md) | Historical plans, specifications, snapshots and validation evidence / 历史归档 |

## Source and data boundaries

- Keep technical findings in `research/`, not hidden session-note folders.
- Keep dated observations and superseded plans in `archive/`; they are
  evidence of earlier work, not instructions for current releases.
- Curated firmware fixtures and their provenance live in
  [tests/fixtures/](../tests/fixtures/README.md), not among documentation.
- Private firmware dumps and scratch data belong in
  [local/](../local/README.md) and are ignored by Git.
- Build outputs remain in `Build/`, `target/` and `dist/`. Existing device
  logs remain in ignored `logs/`; this reorganization does not move them.
- The `zip/` installer submodule is unchanged by this directory migration.
