# Historical design and validation records

| Directory | Contents |
| --- | --- |
| `project/` | Earlier project status, milestone plans and validation runbooks |
| `plans/` | Dated implementation plans and associated task metadata |
| `specs/` | Original design specifications |
| `validation/` | Dated test evidence, including the former hidden RE verification record |

These records preserve the context of earlier work, including superseded
mode names, tool names and assumptions. They are not claims about current
hardware coverage or instructions to execute against a connected device.

References to earlier assistant-specific instructions, hooks and worktree
paths are retained as historical context, not current dependencies.
Current tool-independent guidance lives in [AGENTS.md](../../AGENTS.md).

The plans, specifications and PR evidence in this directory describe the
toolchains available when they were written. Their old Docker commands
are historical, not supported build instructions: Docker support has been
removed. Use the current [English README](../../README.md#building-from-source)
or [中文 README](../../README.zh-CN.md#从源码构建) for the local build workflow.
