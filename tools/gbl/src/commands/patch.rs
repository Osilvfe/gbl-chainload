//! `gbl patch` — host-side ABL patcher.
//!
//! Replaces `tools/abl-patcher/abl-patcher.c`. Post-PR1 contract:
//!
//! - `--no-mode1` is gone (PR1 Task 12 dropped it; `abl_permissive` is
//!   always applied at host packing time).
//! - `--oem oplus` is canonical. `--oem oneplus` is a deprecation alias
//!   that maps to `Oem::Oplus` and prints a one-time warning.
//! - `--check-anchors-only` re-uses the engine's apply loop and only
//!   surfaces a mandatory miss (matching the C source comment about
//!   AMBIGUOUS/MISS being lumped together).
//! - `--mode 2` selects patch11; modes 0/1 do not attempt it.
//! - `--require-patch11` upgrades a mode-2 patch11 miss to a mandatory failure;
//!   mode-2 packaging uses it because 11c is required for custom ROMs.

use std::path::PathBuf;

use clap::Parser;
use patch_engine::{mode2::locked_handoff, Engine, Oem, PatchOutcome, Worst};

use super::{slurp, GblError};

#[derive(Parser, Debug)]
#[command(about = "Apply the host patch table to an ABL PE")]
pub struct Args {
    /// Input PE32+ blob (post fv-unwrap output).
    #[arg(long)]
    r#in: PathBuf,
    /// Output path; omit to skip writing (still applies in-memory).
    #[arg(long)]
    out: Option<PathBuf>,
    /// Anchor-uniqueness probe mode — fail only on a mandatory miss.
    #[arg(long)]
    check_anchors_only: bool,
    /// OEM patch group to apply. `oplus` enables patch7/12/13; `oneplus`
    /// is a deprecation alias; `none` (default) skips all three.
    #[arg(long, value_name = "ID")]
    oem: Option<String>,
    /// Install mode. Only mode 2 enables patch11a/11b/11c.
    #[arg(
        long,
        default_value_t = 0,
        value_parser = clap::value_parser!(u8).range(0..=2)
    )]
    mode: u8,
    /// Deprecated no-op: the disabled-vbmeta cmdline edit is now part of
    /// patch11 (subpatch 11c) and only applied in mode 2.
    #[arg(long, hide = true)]
    experimental_vbmeta_cmdline: bool,
    /// Fail the build unless all three patch11 halves (11a/11b/11c) apply.
    /// Requires --mode 2. Mode-2 packaging enables this automatically.
    #[arg(long)]
    require_patch11: bool,
}

pub fn run(args: Args) -> Result<(), GblError> {
    if args.require_patch11 && args.mode != 2 {
        return Err(GblError::usage("--require-patch11 requires --mode 2"));
    }

    // OEM resolution — mirror abl-patcher.c's `if (OemStr != NULL)` block.
    let oem = match args.oem.as_deref() {
        None => Oem::None,
        Some("oplus") => Oem::Oplus,
        Some("oneplus") => {
            eprintln!(
                "abl-patcher: --oem oneplus is deprecated; use --oem oplus \
                 (accepted for compatibility, will be removed in a future release)"
            );
            Oem::Oplus
        }
        Some("none") => Oem::None,
        Some(other) => {
            return Err(GblError::usage(format!(
                "abl-patcher: unknown --oem '{other}'"
            )));
        }
    };

    // abl_permissive is always on at host packing time.
    let engine = Engine::ensure_init_scoped(oem, true).with_mode2(args.mode == 2);

    let mut buf = slurp(&args.r#in)?;
    let sz = buf.len();
    let mut result = engine.apply(&mut buf);

    if args.experimental_vbmeta_cmdline {
        eprintln!(
            "abl-patcher: --experimental-vbmeta-cmdline is deprecated and \
             no longer needed; patch11c is included in patch11 for --mode 2 only"
        );
    }

    if args.require_patch11 {
        let outcome = locked_handoff::apply(&mut buf, sz as u32);
        let outcome_name = match outcome {
            PatchOutcome::Ok => "OK",
            PatchOutcome::Miss => "MISS",
            PatchOutcome::Ambiguous => "AMBIGUOUS",
        };
        eprintln!(
            "DynamicPatch: patch11-required-custom-rom \
             [mode-2, mandatory] -> {outcome_name}"
        );
        if outcome != PatchOutcome::Ok {
            result.worst = Worst::MandatoryMiss;
        }
    }

    // Aggregate line — byte-for-byte match abl-patcher.c.
    eprintln!(
        "{}: applied={} missed={} worst={} (0=ok 1=optional-miss 2=mandatory-miss)",
        args.r#in.display(),
        result.applied_count,
        result.missed_count,
        worst_as_int(result.worst),
    );

    if args.check_anchors_only {
        if result.worst == Worst::MandatoryMiss {
            eprintln!(
                "FAIL: mandatory patch missed/ambiguous on {}",
                args.r#in.display()
            );
            return Err(GblError::silent(1));
        }
        eprintln!("ok check-anchors-only on {}", args.r#in.display());
        return Ok(());
    }

    if result.worst == Worst::MandatoryMiss {
        eprintln!(
            "ERROR: mandatory patch missed on {}",
            args.r#in.display()
        );
        return Err(GblError::silent(1));
    }

    if let Some(out) = &args.out {
        std::fs::write(out, &buf).map_err(|e| {
            GblError::runtime(format!("{}: write failed: {}", out.display(), e))
        })?;
        eprintln!("wrote {} bytes to {}", sz, out.display());
    }

    Ok(())
}

fn worst_as_int(w: Worst) -> i32 {
    match w {
        Worst::Ok => 0,
        Worst::OptionalMiss => 1,
        Worst::MandatoryMiss => 2,
    }
}
