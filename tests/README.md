# Tests

Run the full host-side suite from the repository root:

```bash
bash tests/runall.sh
```

| Directory | Purpose |
| --- | --- |
| [fixtures/](fixtures/README.md) | Curated, versioned firmware inputs, grouped into `abl/`, `pe/`, `avb/` and `recovery/` |
| `host/` | Host integration and regression scripts |
| `host/helpers/` | Source for native test harnesses; compiled helpers are ignored |
| `host/.last/` | Ignored generated test results and scratch artifacts |
| `avb/` | C ABI tests for the Rust AVB parser |

Use `FIXTURES_DIR=/path/to/fixtures` to override the anchor harness's
fixture root. Both the categorized layout and an existing flat directory
of `.efi`, `.bin` and `.img` files are accepted.

Large recovery fixtures use Git LFS. Run `git lfs pull` before tests that
need their contents; an LFS pointer is not a usable partition image.
Private input firmware belongs in `local/firmware/`, not in the curated
fixture pool. Golden references and optional toolchain prerequisites are
described in the root README. Host tests do not authorize device writes.
