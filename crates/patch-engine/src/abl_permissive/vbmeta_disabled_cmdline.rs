//! patch11c — emit verifier metadata for disabled vbmeta.
//!
//! libavb deliberately skips `avb_append_options` when the top-level
//! vbmeta has `AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED` set.  A
//! verification-disabled image therefore normally reaches userspace
//! without `androidboot.vbmeta.{hash_alg,size,digest}`.  That works while
//! the bootloader reports `orange`, because fs_avb permits verification
//! errors, but it becomes fatal when patch11 reports `green`: userspace
//! treats the handoff as verified and fails in `AvbVerifier::Create()`.
//!
//! This patch makes `avb_slot_verify()` take the normal
//! cmdline path even for a verification-disabled top-level vbmeta.  It
//! does **not** enable verification or process chain descriptors; it only
//! makes libavb publish the digest of the vbmeta image it actually
//! loaded.  fs_avb then sees the disabled flag and skips hashtree setup.
//!
//! The site is a short instruction sequence in `avb_slot_verify`:
//!
//! ```text
//! LDRB W8, [SP,#...]
//! TBNZ W8, #1, <verification-disabled root-only path>
//! LDR  X8, [X23,#0x60]
//! ```
//!
//! The branch word is replaced with NOP.  The LDRB stack offset and TBNZ
//! displacement vary across compiler/library versions, so only their
//! instruction classes are matched; the following `LDR X8,[X23,#0x60]`
//! provides the stable anchor.  The normal path also runs libavb's
//! managed-verity helper, which the disabled path otherwise skips.

use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::PatchOutcome;

/// Stable instruction after the branch: `LDR X8, [X23,#0x60]`.
const ARM64_LDR_X8_X23_60: u32 = 0xF940_32E8;

/// AArch64 `NOP`.
const ARM64_NOP: u32 = 0xD503_201F;

/// `LDRB W8, [SP,#imm]`.
fn is_ldrb_w8_sp(word: u32) -> bool {
    (word & 0xFFC0_0000) == 0x3940_0000
        && (word & 0x1F) == 8
        && ((word >> 5) & 0x1F) == 31
}

/// `TBNZ W8,#1,<label>`.
fn is_tbnz_w8_bit1(word: u32) -> bool {
    (word & 0x7F80_0000) == 0x3700_0000
        && (word & 0x1F) == 8
        && ((word >> 19) & 0x1F) == 1
}

/// Apply the experimental disabled-vbmeta cmdline patch.
///
/// Returns `Ok` when the branch was rewritten or is already NOP.  An
/// absent site returns `Miss`; a duplicate site returns `Ambiguous`.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];
    let mut hit: Option<u32> = None;

    for site_off in (0..active.len().saturating_sub(11)).step_by(4) {
        let site_off = site_off as u32;
        if read_instr_u32(active, site_off + 8) != ARM64_LDR_X8_X23_60 {
            continue;
        }
        let load_byte = read_instr_u32(active, site_off);
        let branch = read_instr_u32(active, site_off + 4);
        if !is_ldrb_w8_sp(load_byte) || !(is_tbnz_w8_bit1(branch) || branch == ARM64_NOP) {
            continue;
        }
        if hit.is_some() {
            return PatchOutcome::Ambiguous;
        }
        hit = Some(site_off);
    }

    let site_off = match hit {
        Some(off) => off,
        None => return PatchOutcome::Miss,
    };
    let branch_off = site_off + 4;
    let word = read_instr_u32(active, branch_off);
    if word == ARM64_NOP {
        return PatchOutcome::Ok;
    }

    write_instr_u32(active, branch_off, ARM64_NOP);
    PatchOutcome::Ok
}

#[cfg(test)]
mod tests {
    use super::*;

    const LDRB_COMPACT: u32 = 0x3943_C3E8;
    const TBNZ_COMPACT: u32 = 0x3708_07A8;
    const LDRB_LARGE_FRAME: u32 = 0x3944_03E8;
    const TBNZ_LARGE_FRAME: u32 = 0x3708_07C8;

    fn site_buffer(load_byte: u32, branch: u32) -> Vec<u8> {
        let mut buf = vec![0u8; 64];
        buf[16..20].copy_from_slice(&load_byte.to_le_bytes());
        buf[20..24].copy_from_slice(&branch.to_le_bytes());
        buf[24..28].copy_from_slice(&ARM64_LDR_X8_X23_60.to_le_bytes());
        buf
    }

    #[test]
    fn rewrites_exact_site_and_is_idempotent() {
        let mut buf = site_buffer(LDRB_COMPACT, TBNZ_COMPACT);
        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 20), ARM64_NOP);
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 20), ARM64_NOP);
    }

    #[test]
    fn rewrites_large_frame_compiler_variant() {
        let mut buf = site_buffer(LDRB_LARGE_FRAME, TBNZ_LARGE_FRAME);
        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 20), ARM64_NOP);
    }

    #[test]
    fn refuses_unexpected_branch() {
        let mut buf = site_buffer(LDRB_COMPACT, 0x3700_07A8);
        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
    }

    #[test]
    fn misses_absent_site() {
        let mut buf = [0u8; 64];
        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
    }
}
