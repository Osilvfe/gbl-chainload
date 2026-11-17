//! patch10 — libavb force-AVB-success.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/libavb_force_success.c`.
//!
//! Anchor on the unique libavb string
//! `"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO"`
//! (verbatim AOSP source text in `avb_slot_verify.c`). From the string xref,
//! locate the enclosing function entry via PACIASP backscan, then apply two
//! rewrites inside `avb_slot_verify`:
//!
//! - **10a** — entry-prologue `mov wN, w3` → `orr wN, w3, #1` (any Rd).
//!   Forces bit 0 of `flags`
//!   (`AVB_SLOT_VERIFY_FLAGS_ALLOW_VERIFICATION_ERROR`) high in the
//!   saved-flags register. All ~7 downstream
//!   `!allow_verification_error` gates inside libavb pass through.
//! - **10c** — exit return-value `mov w0, wM` → `mov w0, #0`. Forces the
//!   function to return `AVB_SLOT_VERIFY_RESULT_OK` regardless of
//!   internal `ret` state.
//!
//! See `docs/research/findings.md`.

use crate::internal::arm64_decode::find_adrp_add_targeting;
use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::internal::pe_sections::is_pe_file_offset_in_executable_section;
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// patch10 anchor string (verbatim AOSP source text from
/// `avb_slot_verify.c:1466`). Used solely as a unique function-locator.
const PATCH10_ANCHOR: &[u8] =
    b"Persistent values required for AVB_HASHTREE_ERROR_MODE_MANAGED_RESTART_AND_EIO";

/// PACIASP — function-entry marker on AArch64 PAC builds.
const ARM64_PACIASP_WORD: u32 = 0xD503_233F;
/// RET.
const ARM64_RET_WORD: u32 = 0xD65F_03C0;
/// `mov w0, #0` (MOVZ w0, #0).
const ARM64_MOV_W0_ZERO: u32 = 0x5280_0000;

/// `mov wN, w3` template (any Rd in low 5 bits).
const ARM64_MOV_FROM_W3_MASK: u32 = 0xFFFF_FFE0;
const ARM64_MOV_FROM_W3_PAT: u32 = 0x2A03_03E0;

/// `orr wN, w3, #1` template — OR in Rd into the low 5 bits.
const ARM64_ORR_W3_ONE_BASE: u32 = 0x3200_0060;

/// `mov w0, wM` template — match on Rd=0, Rn=WZR; Rm wildcarded.
const ARM64_MOV_TO_W0_MASK: u32 = 0xFFE0_FFFF;
const ARM64_MOV_TO_W0_PAT: u32 = 0x2A00_03E0;

/// patch10 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];

    // 1. Find the unique libavb anchor string.
    let (r, str_off) = scan_for(active, PATCH10_ANCHOR, None);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 2. Find the ADRP+ADD pair in .text that loads the string pointer.
    let (r, adrp_off) = find_adrp_add_targeting(active, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 3. Walk backward from ADRP to the nearest PACIASP (function entry).
    let mut func_entry: u32 = 0;
    let mut probe = adrp_off;
    while probe >= 4 {
        let word = read_instr_u32(active, probe - 4);
        if word == ARM64_PACIASP_WORD {
            func_entry = probe - 4;
            break;
        }
        probe -= 4;
    }
    if func_entry == 0 {
        return PatchOutcome::Miss;
    }

    // 4. Forward from func_entry, scan ~30 instructions for `mov wN, w3`.
    //    Stop at the next PACIASP or RET so the scan cannot borrow a match
    //    from the following function. `Rd == 31` is WZR: rewriting a discarded
    //    copy to `orr wzr, w3, #1` would report success without setting the
    //    allow-verification-error bit, so keep looking for a real destination.
    let mut mov_from_w3_off: u32 = 0;
    let scan_limit = func_entry.saturating_add(30 * 4);
    let mut p = func_entry;
    while p + 4 <= scan_limit && (p as usize) + 4 <= active.len() {
        if !is_pe_file_offset_in_executable_section(active, p, 4) {
            break;
        }
        let word = read_instr_u32(active, p);
        if p != func_entry && word == ARM64_PACIASP_WORD {
            break;
        }
        if word == ARM64_RET_WORD {
            break;
        }
        if (word & ARM64_MOV_FROM_W3_MASK) == ARM64_MOV_FROM_W3_PAT && (word & 0x1F) != 31 {
            mov_from_w3_off = p;
            break;
        }
        p += 4;
    }
    if mov_from_w3_off == 0 {
        return PatchOutcome::Miss;
    }

    // 5. Forward from func_entry, scan until first `ret`.
    let mut ret_off: u32 = 0;
    let mut p = func_entry;
    while (p as usize) + 4 <= active.len() {
        if !is_pe_file_offset_in_executable_section(active, p, 4) {
            break;
        }
        let word = read_instr_u32(active, p);
        if p != func_entry && word == ARM64_PACIASP_WORD {
            break;
        }
        if word == ARM64_RET_WORD {
            ret_off = p;
            break;
        }
        p += 4;
    }
    if ret_off == 0 {
        return PatchOutcome::Miss;
    }

    // 6. Walk backward from ret (up to 0x40 bytes) for `mov w0, wM`.
    let mut mov_to_w0_off: u32 = 0;
    let window_floor = ret_off.saturating_sub(0x40);
    let mut p = ret_off;
    while p > func_entry && p > window_floor {
        if !is_pe_file_offset_in_executable_section(active, p - 4, 4) {
            break;
        }
        let word = read_instr_u32(active, p - 4);
        if (word & ARM64_MOV_TO_W0_MASK) == ARM64_MOV_TO_W0_PAT {
            mov_to_w0_off = p - 4;
            break;
        }
        if p < 4 {
            break;
        }
        p -= 4;
    }
    if mov_to_w0_off == 0 {
        return PatchOutcome::Miss;
    }
    if mov_from_w3_off == mov_to_w0_off {
        return PatchOutcome::Miss;
    }

    // 7. Apply both rewrites — preserve Rd from the mov-from-w3.
    let rd = read_instr_u32(active, mov_from_w3_off) & 0x1F;
    let orr_insn = ARM64_ORR_W3_ONE_BASE | rd;
    write_instr_u32(active, mov_from_w3_off, orr_insn);
    write_instr_u32(active, mov_to_w0_off, ARM64_MOV_W0_ZERO);

    PatchOutcome::Ok
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::*;

    const PE_OFF: usize = 0x40;
    const SECTION_OFF: usize = PE_OFF + 24 + 0xE0;
    const TEXT_PTR: u32 = 0x400;
    const TEXT_SIZE: u32 = 0x3000;
    const FUNC: usize = 0x1000;
    const STRING: u32 = 0x2000;

    fn put_u16(buf: &mut [u8], off: usize, value: u16) {
        buf[off..off + 2].copy_from_slice(&value.to_le_bytes());
    }
    fn put_u32(buf: &mut [u8], off: usize, value: u32) {
        buf[off..off + 4].copy_from_slice(&value.to_le_bytes());
    }

    fn synthetic_pe() -> Vec<u8> {
        let mut buf = vec![0u8; 0x4000];
        buf[0] = b'M';
        buf[1] = b'Z';
        put_u32(&mut buf, 0x3C, PE_OFF as u32);
        buf[PE_OFF..PE_OFF + 4].copy_from_slice(b"PE\0\0");
        put_u16(&mut buf, PE_OFF + 6, 1);
        put_u16(&mut buf, PE_OFF + 20, 0xE0);
        buf[SECTION_OFF..SECTION_OFF + 5].copy_from_slice(b".text");
        put_u32(&mut buf, SECTION_OFF + 8, TEXT_SIZE);
        put_u32(&mut buf, SECTION_OFF + 12, TEXT_PTR);
        put_u32(&mut buf, SECTION_OFF + 16, TEXT_SIZE);
        put_u32(&mut buf, SECTION_OFF + 20, TEXT_PTR);
        put_u32(&mut buf, SECTION_OFF + 36, 0x6000_0020);

        let start = STRING as usize;
        buf[start..start + PATCH10_ANCHOR.len()].copy_from_slice(PATCH10_ANCHOR);

        put_u32(&mut buf, FUNC, ARM64_PACIASP_WORD);
        let delta = ((STRING >> 12) as i32) - ((FUNC as u32 >> 12) as i32);
        let imm21 = (delta as u32) & 0x1F_FFFF;
        put_u32(
            &mut buf,
            FUNC + 8,
            0x9000_0000 | ((imm21 & 3) << 29) | (((imm21 >> 2) & 0x7_FFFF) << 5) | 8,
        );
        put_u32(
            &mut buf,
            FUNC + 12,
            0x9100_0000 | ((STRING & 0xFFF) << 10) | (8 << 5) | 8,
        );
        put_u32(&mut buf, FUNC + 0x20, ARM64_RET_WORD);
        put_u32(&mut buf, FUNC + 0x1C, 0x2A08_03E0); // mov w0, w8
        buf
    }

    fn apply_to(buf: &mut [u8]) -> PatchOutcome {
        let size = buf.len() as u32;
        apply(buf, size)
    }

    #[test]
    fn skips_wzr_then_uses_a_real_destination() {
        let mut buf = synthetic_pe();
        put_u32(&mut buf, FUNC + 4, 0x2A03_03FF); // mov wzr, w3 — must be skipped
        put_u32(&mut buf, FUNC + 0x10, 0x2A03_03E1); // mov w1, w3

        assert_eq!(apply_to(&mut buf), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, (FUNC + 4) as u32), 0x2A03_03FF);
        assert_eq!(read_instr_u32(&buf, (FUNC + 0x10) as u32), 0x3200_0061);
        assert_eq!(
            read_instr_u32(&buf, (FUNC + 0x1C) as u32),
            ARM64_MOV_W0_ZERO
        );
    }

    #[test]
    fn only_wzr_is_a_miss_without_writes() {
        let mut buf = synthetic_pe();
        put_u32(&mut buf, FUNC + 4, 0x2A03_03FF);
        let before = buf.clone();

        assert_eq!(apply_to(&mut buf), PatchOutcome::Miss);
        assert_eq!(buf, before);
    }

    #[test]
    fn overlapping_flag_and_return_sites_are_rejected() {
        let mut buf = synthetic_pe();
        // Remove the normal `mov w0, w8` and make `mov w0, w3` the only
        // candidate for both the flag copy and the return value.
        put_u32(&mut buf, FUNC + 0x1C, ARM64_PACIASP_WORD);
        put_u32(&mut buf, FUNC + 0x18, 0x2A03_03E0);
        let before = buf.clone();

        assert_eq!(apply_to(&mut buf), PatchOutcome::Miss);
        assert_eq!(buf, before);
    }

    #[test]
    fn tracked_infiniti_fixture_still_applies() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../tests/fixtures/pe/infiniti-EU-16.0.5.703.efi");
        let Ok(data) = std::fs::read(&path) else {
            return;
        };
        let mut buf = data.clone();
        let size = buf.len() as u32;

        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        let mut orr = 0u32;
        let mut off = 0u32;
        while (off as usize) + 4 <= buf.len() {
            let word = read_instr_u32(&buf, off);
            if (word & ARM64_MOV_FROM_W3_MASK) == ARM64_ORR_W3_ONE_BASE {
                assert_ne!(word & 0x1F, 31, "wrote ORR WZR at 0x{off:x}");
                orr += 1;
            }
            off += 4;
        }
        assert_eq!(orr, 1, "expected exactly one forced-flags ORR");
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
    }
}
