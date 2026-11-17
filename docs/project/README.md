# Project records

This directory holds project decisions and upstream issue tracking.
Current build and usage instructions live in the root READMEs; technical
research is indexed separately in [research/](../research/README.md).

## Files

- [`decisions.md`](decisions.md) — durable decisions and rejected paths.
- [`upstream-issues.md`](upstream-issues.md) — snapshot and disposition of open upstream issues/PRs.

Earlier [state snapshots](../archive/project/current-state.md),
[milestone plans](../archive/project/next-milestone.md) and the
[recovery validation runbook](../archive/project/recovery-install-validation.md)
are retained as history, not current status reports.

## Documentation policy

- Keep project decisions and dated issue snapshots here.
- Put distilled technical facts in `docs/research/findings.md`; private
  transcripts and raw captures belong in ignored `local/` or `logs/`.
- Preserve superseded plans and dated validation evidence in `docs/archive/`
  without presenting them as current instructions.
- Device-risky test flows remain bounded by [AGENTS.md](../../AGENTS.md): RAM-load with `fastboot stage dist/<artifact>.efi` and `fastboot oem boot-efi`; do not flash non-HLOS images autonomously.
