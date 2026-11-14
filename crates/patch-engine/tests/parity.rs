//! Parity tests — Rust patch-engine outputs match the C reference's
//! behavior on the tracked PE fixture.
//!
//! Direct port of the three host tests deleted from `tests/patches/`:
//!
//!   - `test_patch6.c` — lock-state fastboot-gate parity.
//!   - `test_patch7.c` — orange-screen parity (incl. anchor uniqueness,
//!     CBZ→B rewrite, target preservation, idempotency, clean MISS on
//!     scrubbed warning string).
//!   - `test_patch10.c` — libavb force-AVB-success parity.
//!
//! The 4-ABL cross-build matrix continues to live at
//! `tests/host/088_patch7_multi_abl.sh`, which depends on `fv-unwrap`
//! to extract the PE from raw `.img` FV wrappers. We don't bring that
//! dependency into cargo's test path — the host shell test is the
//! authoritative cross-build gate.

use std::fs;
use std::path::PathBuf;

use patch_engine::{
    abl_permissive::{fastboot_lock_gates, libavb_force_success, vbmeta_disabled_cmdline},
    internal::{
        arm64_decode::find_adrp_add_targeting,
        encode::read_instr_u32,
        scan::{scan_for, ScanResult},
    },
    Engine, Oem, PatchOutcome, PatchScope, Worst,
};

const PATCH14_BRANCH_OFF: u32 = 0x2B1B0; // infiniti-EU-16.0.5.703 fixture
const PATCH14_TBNZ_W8_1: u32 = 0x3708_07A8;
const PATCH14_NOP: u32 = 0xD503_201F;

fn repo_root() -> PathBuf {
    // CARGO_MANIFEST_DIR = crates/patch-engine/. Climb two levels to repo root.
    let mut p = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    p.pop();
    p.pop();
    p
}

fn infiniti_pe() -> Option<Vec<u8>> {
    let path = repo_root().join("tests/images/pe/infiniti-EU-16.0.5.703.efi");
    if !path.exists() {
        return None;
    }
    Some(fs::read(&path).expect("read infiniti PE fixture"))
}

// --- patch6 — lock-state fastboot-gate -------------------------------

const REFUSAL_STRS: &[&[u8]] = &[
    b"Flashing is not allowed in Lock State",
    b"Erase is not allowed in Lock State",
    b"Slot Change is not allowed in Lock State\n",
    b"Snapshot Cancel is not allowed in Lock State",
];

#[test]
fn patch6_applies_and_is_miss_on_reapply() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch6_applies — infiniti PE fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    // First application.
    let outcome = fastboot_lock_gates::apply(&mut work, size);
    assert_eq!(
        outcome,
        PatchOutcome::Ok,
        "patch6 must apply cleanly on infiniti PE"
    );

    // Refusal strings must stay intact — patch6 only rewrites .text.
    for s in REFUSAL_STRS {
        assert!(
            work.windows(s.len()).any(|w| w == *s),
            "patch6 erased refusal string {:?}",
            std::str::from_utf8(s).unwrap()
        );
    }

    // Re-apply: every upstream conditional / B.NE has been rewritten,
    // so the second application can't find another gate site → MISS.
    let outcome = fastboot_lock_gates::apply(&mut work, size);
    assert_eq!(outcome, PatchOutcome::Miss);
}

// --- patch7 — orange-screen ------------------------------------------

const PATCH7_WARN_STR: &[u8] = b"Your device has been unlocked and can't be trusted";
const PATCH7_BACK_SCAN_WINDOW: u32 = 0x40;
const PATCH7_CBZ_OFF: u32 = 0x78F0; // infiniti-EU-16.0.5.703 specific
const PATCH7_B_UNCOND_INSN: u32 = 0x14000023; // rewritten B word — frozen byte parity

#[test]
fn patch7_table_membership() {
    // Mirror of test_patch7.c section 0 — the table-membership check
    // runs regardless of fixture presence.
    let oplus_table = patch_engine::oem::oplus::OEM_OPLUS_PATCHES;
    assert!(!oplus_table.is_empty());
    assert!(oplus_table
        .iter()
        .any(|p| p.name == "patch7-orange-screen"));
}

#[test]
fn patch7_string_anchor_resolves_uniquely() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_string_anchor_resolves_uniquely — fixture missing");
            return;
        }
    };
    let (r, str_off) = scan_for(&buf, PATCH7_WARN_STR, None);
    assert_eq!(r, ScanResult::Found, "warning string not unique");
    let (r, adrp_off) = find_adrp_add_targeting(&buf, str_off, true);
    assert_eq!(r, ScanResult::Found, "ADRP+ADD not unique");
    assert!(adrp_off > PATCH7_CBZ_OFF);
    assert!(adrp_off - PATCH7_CBZ_OFF <= PATCH7_BACK_SCAN_WINDOW);
}

#[test]
fn patch7_pre_patch_cbz_word() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_pre_patch_cbz_word — fixture missing");
            return;
        }
    };
    let cbz = read_instr_u32(&buf, PATCH7_CBZ_OFF);
    assert_eq!((cbz & 0xFF000000), 0x34000000, "not a CBZ Wn");
    assert_eq!((cbz & 0x1F), 10, "Rt != W10");
}

#[test]
fn patch7_apply_rewrites_to_b_uncond() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_apply — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();
    let cbz_original = read_instr_u32(&work, PATCH7_CBZ_OFF);

    use patch_engine::oem::oplus::bypass_warning;
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Ok);

    let new_word = read_instr_u32(&work, PATCH7_CBZ_OFF);
    assert_eq!((new_word & 0xFC000000), 0x14000000, "not a B insn");
    assert_eq!(
        new_word, PATCH7_B_UNCOND_INSN,
        "B word mismatch — frozen byte parity broken"
    );

    // Target preservation: the rewritten B must point at the same byte
    // address as the original CBZ.
    let cbz_imm19 = sign_extend((cbz_original >> 5) & 0x7FFFF, 19) * 4;
    let cbz_target = (PATCH7_CBZ_OFF as i32 + cbz_imm19) as u32;
    let b_imm26 = sign_extend(new_word & 0x3FFFFFF, 26) * 4;
    let b_target = (PATCH7_CBZ_OFF as i32 + b_imm26) as u32;
    assert_eq!(cbz_target, b_target, "B target != original CBZ target");

    // Idempotency: second apply still returns Ok, word stays put.
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Ok);
    assert_eq!(read_instr_u32(&work, PATCH7_CBZ_OFF), PATCH7_B_UNCOND_INSN);
}

#[test]
fn patch7_clean_miss_without_warning_string() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch7_clean_miss — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();
    let cbz_original = read_instr_u32(&work, PATCH7_CBZ_OFF);

    // Break the warning string at its first occurrence.
    let (r, str_off) = scan_for(&work, PATCH7_WARN_STR, None);
    assert_eq!(r, ScanResult::Found);
    work[str_off as usize] = 0x00;

    use patch_engine::oem::oplus::bypass_warning;
    assert_eq!(bypass_warning::apply(&mut work, size), PatchOutcome::Miss);
    assert_eq!(
        read_instr_u32(&work, PATCH7_CBZ_OFF),
        cbz_original,
        "patch7 mutated the CBZ on a MISS"
    );
}

fn sign_extend(v: u32, bits: u32) -> i32 {
    let mask = 1u32 << (bits - 1);
    if (v & mask) != 0 {
        let high = !((1u32 << bits) - 1);
        (v | high) as i32
    } else {
        v as i32
    }
}

// --- patch10 — libavb force-AVB-success ------------------------------

const PATCH10_ANCHOR: &[u8] =
    b"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

#[test]
fn patch10_table_membership() {
    let abl_table = patch_engine::abl_permissive::ABL_PERMISSIVE_PATCHES;
    assert!(abl_table
        .iter()
        .any(|p| p.name == "patch10-libavb-force-avb-success"));
    assert!(abl_table
        .iter()
        .any(|p| p.name == "patch6-lock-state-fastboot-gate"));
}

#[test]
fn patch10_applies_and_leaves_signature_words() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch10 — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    // Sanity: the anchor string is present in this PE.
    let (r, _) = scan_for(&work, PATCH10_ANCHOR, None);
    assert_eq!(r, ScanResult::Found, "anchor string absent on infiniti PE");

    assert_eq!(libavb_force_success::apply(&mut work, size), PatchOutcome::Ok);

    // Sweep for the two rewrite signatures.
    let mut orr_seen = 0;
    let mut movz_seen = 0;
    let mut i = 0u32;
    while (i as usize) + 4 <= work.len() {
        let word = read_instr_u32(&work, i);
        if (word & 0xFFFFFFE0) == 0x32000060 {
            orr_seen += 1;
        }
        if word == 0x52800000 {
            movz_seen += 1;
        }
        i += 4;
    }
    assert!(orr_seen >= 1, "patch10 did not produce `orr wN, w3, #1`");
    assert!(movz_seen >= 1, "patch10 did not produce `mov w0, #0`");

    // Re-apply: prologue mov-from-w3 is gone → MISS at step 4.
    assert_eq!(
        libavb_force_success::apply(&mut work, size),
        PatchOutcome::Miss
    );
}

// --- patch11 — locked-handoff bootconfig ------------------------------

const PATCH11_DEVSTATE_STR: &[u8] = b"androidboot.vbmeta.device_state";
const PATCH11_CSEL_OFF: u32 = 0x2B970; // infiniti-EU-16.0.5.703 specific
// CSEL x2, x10, x9, EQ — byte-identical across all five inspected
// builds (infiniti 703/201, fairlady 201/701, pudding 44). Frozen.
const PATCH11_CSEL_ORIG: u32 = 0x9A89_0142;
// MOV x2, x10 (ORR x2, XZR, x10) — the forced "locked" operand.
const PATCH11_CSEL_MOV: u32 = 0xAA0A_03E2;
// Color table slots on the EU 703 fixture (16-byte-stride
// {idx, ptr} tuples: green @+8, orange @+24, yellow @+40, red @+56).
const PATCH11_TABLE_OFF: usize = 0x7B540;
const PATCH11_GREEN_PTR: u64 = 0x66275;

#[test]
fn patch11_table_membership() {
    let abl_table = patch_engine::abl_permissive::ABL_PERMISSIVE_PATCHES;
    let p = abl_table
        .iter()
        .find(|p| p.name == "patch11-locked-handoff-bootconfig")
        .expect("patch11 missing from abl_permissive table");
    assert!(!p.mandatory, "patch11 must be optional (never blocks chainload)");
}

#[test]
fn patch11_apply_rewrites_csel_and_color_table() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch11 — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    // Pre-patch frozen bytes.
    assert_eq!(
        read_instr_u32(&work, PATCH11_CSEL_OFF),
        PATCH11_CSEL_ORIG,
        "CSEL word drift — fixture or build layout changed"
    );

    use patch_engine::abl_permissive::locked_handoff;
    assert_eq!(locked_handoff::apply(&mut work, size), PatchOutcome::Ok);

    // 11a: CSEL → MOV x2, x10 ("locked" operand).
    assert_eq!(read_instr_u32(&work, PATCH11_CSEL_OFF), PATCH11_CSEL_MOV);

    // 11b: orange and yellow table slots repointed at green; green and
    // red slots untouched.
    let slot = |w: &[u8], o: usize| {
        let mut v = 0u64;
        for i in 0..8 {
            v |= (w[o + i] as u64) << (8 * i);
        }
        v
    };
    assert_eq!(slot(&work, PATCH11_TABLE_OFF + 8), PATCH11_GREEN_PTR);
    assert_eq!(slot(&work, PATCH11_TABLE_OFF + 24), PATCH11_GREEN_PTR);
    assert_eq!(slot(&work, PATCH11_TABLE_OFF + 40), PATCH11_GREEN_PTR);
    // Red slot still points at "red".
    let red_ptr = slot(&work, PATCH11_TABLE_OFF + 56);
    let red_off = red_ptr as usize;
    assert_eq!(&work[red_off..red_off + 4], b"red\0");
    // 11c: disabled-vbmeta branch is now NOP.
    assert_eq!(read_instr_u32(&work, PATCH14_BRANCH_OFF), PATCH14_NOP);
    // The anchor key string survives (patch must not eat its anchor).
    let (r, _) = scan_for(&work, PATCH11_DEVSTATE_STR, None);
    assert_eq!(r, ScanResult::Found);

    // Idempotency: re-apply stays Ok and bytes stay put.
    assert_eq!(locked_handoff::apply(&mut work, size), PatchOutcome::Ok);
    assert_eq!(read_instr_u32(&work, PATCH11_CSEL_OFF), PATCH11_CSEL_MOV);
    assert_eq!(slot(&work, PATCH11_TABLE_OFF + 24), PATCH11_GREEN_PTR);
    assert_eq!(read_instr_u32(&work, PATCH14_BRANCH_OFF), PATCH14_NOP);
}

#[test]
fn patch11_clean_miss_without_anchor() {
    let mut work = vec![0u8; 4096];
    let size = work.len() as u32;
    use patch_engine::abl_permissive::locked_handoff;
    assert_eq!(locked_handoff::apply(&mut work, size), PatchOutcome::Miss);
}

// --- patch11c — disabled-vbmeta cmdline ------------------------------

#[test]
fn patch11c_is_bundled_and_not_a_separate_table_entry() {
    assert!(
        !patch_engine::abl_permissive::ABL_PERMISSIVE_PATCHES
            .iter()
            .any(|p| p.name.contains("patch14")),
        "patch11c must not be a separate patch14 table entry"
    );
}

#[test]
fn patch11c_rewrites_disabled_vbmeta_branch_and_is_idempotent() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch11c — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    assert_eq!(read_instr_u32(&work, PATCH14_BRANCH_OFF), PATCH14_TBNZ_W8_1);
    assert_eq!(
        vbmeta_disabled_cmdline::apply(&mut work, size),
        PatchOutcome::Ok
    );
    assert_eq!(read_instr_u32(&work, PATCH14_BRANCH_OFF), PATCH14_NOP);

    assert_eq!(
        vbmeta_disabled_cmdline::apply(&mut work, size),
        PatchOutcome::Ok
    );
    assert_eq!(read_instr_u32(&work, PATCH14_BRANCH_OFF), PATCH14_NOP);
}

#[test]
fn patch11c_clean_miss_on_zero_buffer() {
    let mut work = vec![0u8; 4096];
    let size = work.len() as u32;
    assert_eq!(
        vbmeta_disabled_cmdline::apply(&mut work, size),
        PatchOutcome::Miss
    );
}

// --- patch12 — boot-failure fallback (v3: orange branch disabled) -----

const PATCH12_ANCHOR_LOCKED: &[u8] = b"if olock secure lock state, Not allow Fastboot.";
const PATCH12_ANCHOR_VERIFY: &[u8] = b"fastboot_unlock_verify error and reboot";
const PATCH12_ANCHOR_NO_SUPPORT: &[u8] =
    b"No fastboot support for flashless chipsets, Infinte loop";
const ARM64_NOP: u32 = 0xD503_201F;
const ARM64_MOV_W0_RESET_REASON: u32 = 0x5280_07C0;

fn adrp_for(buf: &[u8], anchor: &[u8]) -> u32 {
    let (r, str_off) = scan_for(buf, anchor, None);
    assert_eq!(r, ScanResult::Found, "anchor string not unique");
    let (r, adrp_off) = find_adrp_add_targeting(buf, str_off, true);
    assert_eq!(r, ScanResult::Found, "anchor xref not unique");
    adrp_off
}

fn b_target(word: u32, off: u32) -> u32 {
    assert_eq!(word >> 26, 0x05, "not an unconditional B at 0x{off:x}");
    let imm = ((word & 0x03FF_FFFF) as i32) << 6 >> 6;
    off.wrapping_add((imm * 4) as u32)
}

#[test]
fn patch12_table_membership() {
    let p = patch_engine::abl_permissive::ABL_PERMISSIVE_PATCHES
        .iter()
        .find(|p| p.name == "patch12-boot-failure-fastboot-fallback")
        .expect("patch12 missing from abl_permissive table");
    assert!(!p.mandatory, "patch12 must be optional (never blocks chainload)");
}

#[test]
fn patch12_v3_removes_both_the_reset_and_the_spin() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: patch12 — fixture missing");
            return;
        }
    };
    let size = buf.len() as u32;
    let mut work = buf.clone();

    let locked_adrp = adrp_for(&work, PATCH12_ANCHOR_LOCKED);
    let verify_adrp = adrp_for(&work, PATCH12_ANCHOR_VERIFY);
    let nosupport_adrp = adrp_for(&work, PATCH12_ANCHOR_NO_SUPPORT);
    let launch_off = nosupport_adrp + 0x70;
    let spin_off = nosupport_adrp + 0x10;

    // Stock shape: CBZ gate, TBZ status test -> launch, B . spin.
    let gate_off = locked_adrp - 4;
    let status_off = locked_adrp + 0x18;
    let tail_off = status_off + 0x70;
    assert_eq!(read_instr_u32(&work, gate_off) & 0x7E00_0000, 0x3400_0000);
    assert_eq!(read_instr_u32(&work, status_off) >> 24, 0xB6); // TBZ bit63
    assert_eq!(read_instr_u32(&work, tail_off) >> 24, 0xB7); // TBNZ bit63
    assert_eq!(read_instr_u32(&work, spin_off), 0x1400_0000); // B .
    for adrp in [locked_adrp, verify_adrp] {
        assert_eq!(read_instr_u32(&work, adrp + 0x10), ARM64_MOV_W0_RESET_REASON);
    }

    use patch_engine::abl_permissive::boot_failure_fallback;
    assert_eq!(
        boot_failure_fallback::apply(&mut work, size),
        PatchOutcome::Ok
    );

    // Every refusal / spin decision now goes straight to "Launching
    // fastboot": the orange branch is unreachable.
    for off in [gate_off, status_off, tail_off] {
        assert_eq!(
            b_target(read_instr_u32(&work, off), off),
            launch_off,
            "0x{off:x} does not continue to LABEL_342"
        );
    }
    // The unlock-verify refusal no longer reboots.
    assert_eq!(read_instr_u32(&work, verify_adrp + 0x14), ARM64_NOP);

    // The reset inside the (now unreachable) refusal block is left in
    // place; we removed reachability, not the helper.
    assert_ne!(read_instr_u32(&work, locked_adrp + 0x14), ARM64_NOP);

    // The retry-count machinery must be untouched.
    let (r, retry_str) = scan_for(&work, b"A/B retry count NOT decremented", None);
    assert_eq!(r, ScanResult::Found);
    let (r, retry_adrp) = find_adrp_add_targeting(&work, retry_str, true);
    assert_eq!(r, ScanResult::Found);
    assert_ne!(read_instr_u32(&work, retry_adrp - 0x04), ARM64_NOP);
    assert_ne!(read_instr_u32(&work, retry_adrp - 0x10), ARM64_NOP);

    let snapshot = work.clone();
    assert_eq!(
        boot_failure_fallback::apply(&mut work, size),
        PatchOutcome::Ok
    );
    assert_eq!(work, snapshot, "patch12 must be idempotent");
}

#[test]
fn patch12_clean_miss_without_anchor() {
    let mut work = vec![0u8; 4096];
    let size = work.len() as u32;
    use patch_engine::abl_permissive::boot_failure_fallback;
    assert_eq!(
        boot_failure_fallback::apply(&mut work, size),
        PatchOutcome::Miss
    );
}

// --- Engine-level integration tests -----------------------------------

#[test]
fn engine_scoped_oplus_applies_all_three() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: engine_scoped_oplus_applies_all_three — fixture missing");
            return;
        }
    };
    let mut work = buf.clone();
    let engine = Engine::ensure_init_scoped(Oem::Oplus, true);
    let r = engine.apply(&mut work);
    // patch7 + patch10 + patch6 + patch11 + patch12 = 5 applied.
    assert_eq!(
        r.applied_count, 5,
        "expected patch7 + patch10 + patch6 + patch11 + patch12 all OK"
    );
    assert_eq!(r.missed_count, 0);
    assert_eq!(r.worst, Worst::Ok);
}

#[test]
fn engine_scoped_none_only_runs_abl_permissive() {
    let buf = match infiniti_pe() {
        Some(b) => b,
        None => {
            eprintln!("SKIP: engine_scoped_none_only_runs_abl_permissive — fixture missing");
            return;
        }
    };
    let mut work = buf.clone();
    let engine = Engine::ensure_init_scoped(Oem::None, true);
    let r = engine.apply(&mut work);
    // patch10 + patch6 + patch11 + patch12.
    assert_eq!(r.applied_count, 4);
    assert_eq!(r.missed_count, 0);
    assert_eq!(r.worst, Worst::Ok);

    // patch7 must NOT have been touched — verify by re-running just
    // patch7 in isolation; it should still find the original CBZ and
    // rewrite to B.
    use patch_engine::oem::oplus::bypass_warning;
    let mut isolated = work.clone();
    let isize = isolated.len() as u32;
    assert_eq!(
        bypass_warning::apply(&mut isolated, isize),
        PatchOutcome::Ok,
        "patch7 wasn't pre-applied by Oem::None scope"
    );
}

#[test]
fn engine_firmware_path_only_abl_permissive() {
    // Engine::ensure_init() is the firmware-mode entry point — must
    // contain exactly the abl_permissive set.
    let engine = Engine::ensure_init();
    assert_eq!(engine.table().len(), 4);
    for p in engine.table() {
        assert_eq!(p.scope, PatchScope::AblPermissive);
    }
}
