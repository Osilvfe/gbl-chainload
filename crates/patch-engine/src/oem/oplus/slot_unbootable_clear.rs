//! Oplus patch13 — never treat an A/B slot as unbootable (read-side only).
//!
//! OPPO's ABL keeps one attribute word per slot in the GPT partition
//! entry's 64-bit `Attributes` field (`boot_a` / `boot_b`). The high
//! dword of that field is what ABL manipulates, so relative to the dword
//! at `entry+0x34`:
//!
//! ```text
//!   bits 16..17  priority
//!   bit  18      active
//!   bits 19..21  retry count
//!   bit  22      successful
//!   bit  23      unbootable
//! ```
//!
//! `GetSlotInfoByName` hands that word out to everything — the slot
//! decision logic, `IsCurrentSlotBootable`, and the GPT serializer. Once
//! ABL sets bit 23 (retry exhaustion, `set_active`, a refused verify) the
//! slot stays flagged until something clears it, and a device whose slots
//! are both flagged has no path back into Android.
//!
//! This patch makes the stored flag inert for **this boot only**. It
//! deliberately leaves the GPT write path stock:
//!
//! 1. **Predicate helper** — the tiny leaf
//!    `MOV X9, #0x40000000000000; LSL X8, X8, #32;
//!     AND X8, X8, #0xC0000000000000; CMP X8, X9; RET`
//!    returns "successful && !unbootable". Its `LSL` is rewritten to
//!    `MOV X8, X9`, which makes the following `AND`+`CMP` always
//!    compare equal: every caller (`IsCurrentSlotBootable`, the
//!    `ClearUnbootable` precondition, and the boot-image slot check)
//!    sees a bootable slot before any later GPT write lands.
//! 2. **Decision read** — `UBFX X24, X27, #0x17, #1` (the unbootable
//!    bit feeding the retry/alternate-slot decision) becomes
//!    `MOV X24, XZR`.
//! 3. **Counting read** — `UBFX W8, W8, #0x17, #1` (the "how many
//!    slots are unbootable" accumulator) becomes `MOV W8, WZR`.
//!
//! The setter (`ORR W9, W9, #0x800000`) and the GPT serializer
//! (`LDRH W8, [X9, #6]`) are intentionally **not** rewritten. The
//! on-disk `unbootable` bit may therefore remain set, but it cannot
//! affect the current boot's slot decisions. Avoiding the serializer
//! edit also avoids turning every GPT metadata write into a write that
//! silently clears a persistent flag.
//!
//! All sites are located by exact instruction words/sequences and must
//! be unique in the image; anything unexpected yields `Miss` so the
//! chainload never ships a half-applied patch. Idempotent: a rewritten
//! site no longer matches its stock encoding and is recognised as
//! already applied.
//!
//! Interaction with patch12: patch12 owns the *reset* exits, patch13
//! owns the *state*. Neither depends on the other's bytes.

use crate::internal::encode::write_instr_u32;
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// The slot-bootable predicate leaf (see module docs).
const PREDICATE: [u32; 5] = [
    0xD2E0_0809, // MOV  X9, #0x40000000000000
    0xD360_7D08, // LSL  X8, X8, #0x20
    0x924A_0508, // AND  X8, X8, #0xC0000000000000
    0xEB09_011F, // CMP  X8, X9
    0xD65F_03C0, // RET
];
/// Index of the `LSL` in [`PREDICATE`].
const PREDICATE_LSL_IDX: usize = 1;
/// `MOV X8, X9` — the replacement for the `LSL`.
const ARM64_MOV_X8_X9: u32 = 0xAA09_03E8;

/// `UBFX X24, X27, #0x17, #1` — unbootable bit into the slot decision.
const ARM64_UBFX_DECISION: u32 = 0xD357_5F78;
/// `UBFX X23, X27, #0x16, #1` — the adjacent "successful" read, used
/// only to make the decision site unique.
const ARM64_UBFX_SUCCESS: u32 = 0xD356_5B77;
/// `MOV X24, XZR`.
const ARM64_MOV_X24_XZR: u32 = 0xAA1F_03F8;

/// `ADD X19, X19, #6` — walks the slot-name table.
const ARM64_ADVANCE_SLOT: u32 = 0x9100_1A73;
/// `UBFX W8, W8, #0x17, #1` — unbootable bit into the slot counter.
const ARM64_UBFX_COUNT: u32 = 0x5317_5D08;
/// `MOV W8, WZR`.
const ARM64_MOV_W8_WZR: u32 = 0x2A1F_03E8;

/// Find the unique location of an instruction sequence (max 8 words —
/// every sequence this patch looks for is 3 or 5 words).
fn find_sequence(buf: &[u8], words: &[u32]) -> Result<u32, PatchOutcome> {
    const MAX_WORDS: usize = 8;
    let mut bytes = [0u8; MAX_WORDS * 4];
    let n = words.len();
    if n == 0 || n > MAX_WORDS {
        return Err(PatchOutcome::Miss);
    }
    for (i, w) in words.iter().enumerate() {
        bytes[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
    }
    let (r, off) = scan_for(buf, &bytes[..n * 4], None);
    match r {
        ScanResult::Found => Ok(off),
        ScanResult::Ambiguous => Err(PatchOutcome::Ambiguous),
        _ => Err(PatchOutcome::Miss),
    }
}

/// Resolved edits for one image.
struct Plan {
    /// `LSL` inside the bootable predicate.
    predicate_off: u32,
    predicate_done: bool,
    /// `UBFX` feeding the slot decision.
    decision_off: u32,
    decision_done: bool,
    /// `UBFX` feeding the slot counter.
    count_off: u32,
    count_done: bool,
}

/// Locate one site: `stock` and `patched` are the same sequence with
/// exactly one differing word at `idx`. Returns `(file_off, already)`.
fn locate_site(
    active: &[u8],
    stock: &[u32],
    patched: &[u32],
    idx: usize,
) -> Result<(u32, bool), PatchOutcome> {
    match find_sequence(active, stock) {
        Ok(off) => Ok((off + (idx as u32) * 4, false)),
        Err(PatchOutcome::Miss) => {
            let off = find_sequence(active, patched)?;
            Ok((off + (idx as u32) * 4, true))
        }
        Err(e) => Err(e),
    }
}

fn plan(active: &[u8]) -> Result<Plan, PatchOutcome> {
    // 1. Predicate leaf — 5-word block, only the LSL changes.
    let mut predicate_patched = PREDICATE;
    predicate_patched[PREDICATE_LSL_IDX] = ARM64_MOV_X8_X9;
    let (predicate_off, predicate_done) =
        locate_site(active, &PREDICATE, &predicate_patched, PREDICATE_LSL_IDX)?;

    // 2. Decision read — paired with the adjacent "successful" read so
    //    the replacement (`MOV X24, XZR`) need not be unique on its own.
    let decision_stock = [ARM64_UBFX_DECISION, ARM64_UBFX_SUCCESS];
    let decision_patched = [ARM64_MOV_X24_XZR, ARM64_UBFX_SUCCESS];
    let (decision_off, decision_done) =
        locate_site(active, &decision_stock, &decision_patched, 0)?;

    // 3. Counter read — paired with the loop's slot-table advance.
    let count_stock = [ARM64_ADVANCE_SLOT, ARM64_UBFX_COUNT];
    let count_patched = [ARM64_ADVANCE_SLOT, ARM64_MOV_W8_WZR];
    let (count_off, count_done) = locate_site(active, &count_stock, &count_patched, 1)?;

    Ok(Plan {
        predicate_off,
        predicate_done,
        decision_off,
        decision_done,
        count_off,
        count_done,
    })
}

/// patch13 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];
    let p = match plan(active) {
        Ok(p) => p,
        Err(e) => return e,
    };

    if !p.predicate_done {
        write_instr_u32(active, p.predicate_off, ARM64_MOV_X8_X9);
    }
    if !p.decision_done {
        write_instr_u32(active, p.decision_off, ARM64_MOV_X24_XZR);
    }
    if !p.count_done {
        write_instr_u32(active, p.count_off, ARM64_MOV_W8_WZR);
    }

    PatchOutcome::Ok
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::internal::encode::read_instr_u32;

    const ARM64_SET_UNBOOTABLE: u32 = 0x3209_0129; // ORR W9, W9, #0x800000
    const ARM64_LDP_ATTR: u32 = 0x29C6_2688; // LDP W8, W9, [X20, #0x30]!
    const ARM64_STP_ATTR: u32 = 0x2900_2688; // STP W8, W9, [X20]
    const ARM64_SERIALIZE_ATTR_HI: u32 = 0x7940_0D28; // LDRH W8, [X9, #6]

    #[test]
    fn read_back_after_apply_is_idempotent_and_leaves_write_sites_stock() {
        let mut img = vec![0u8; 0x400];
        let put = |img: &mut Vec<u8>, off: usize, w: u32| {
            img[off..off + 4].copy_from_slice(&w.to_le_bytes());
        };
        put(&mut img, 0x00, ARM64_LDP_ATTR);
        put(&mut img, 0x04, ARM64_SET_UNBOOTABLE);
        put(&mut img, 0x08, ARM64_STP_ATTR);
        put(&mut img, 0x20, ARM64_SERIALIZE_ATTR_HI);
        for (i, w) in PREDICATE.iter().enumerate() {
            put(&mut img, 0x40 + i * 4, *w);
        }
        put(&mut img, 0x100, ARM64_UBFX_DECISION);
        put(&mut img, 0x104, ARM64_UBFX_SUCCESS);
        put(&mut img, 0x200, ARM64_ADVANCE_SLOT);
        put(&mut img, 0x204, ARM64_UBFX_COUNT);

        let size = img.len() as u32;
        assert_eq!(apply(&mut img, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&img, 0x44), ARM64_MOV_X8_X9);
        assert_eq!(read_instr_u32(&img, 0x100), ARM64_MOV_X24_XZR);
        assert_eq!(read_instr_u32(&img, 0x204), ARM64_MOV_W8_WZR);

        // The GPT write path is deliberately untouched.
        assert_eq!(read_instr_u32(&img, 0x04), ARM64_SET_UNBOOTABLE);
        assert_eq!(read_instr_u32(&img, 0x20), ARM64_SERIALIZE_ATTR_HI);

        let snapshot = img.clone();
        assert_eq!(apply(&mut img, size), PatchOutcome::Ok);
        assert_eq!(img, snapshot, "patch13 must be idempotent");
    }

    #[test]
    fn missing_read_sites_are_a_clean_miss() {
        let mut img = vec![0u8; 0x400];
        let size = img.len() as u32;
        assert_eq!(apply(&mut img, size), PatchOutcome::Miss);
    }
}
