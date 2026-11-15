//! patch13 — never treat an A/B slot as unbootable.
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
//! decision logic, `IsCurrentSlotBootable`, and the GPT serializer. Once ABL sets bit 23 (retry exhaustion, `set_active`,
//! a refused verify) the slot stays flagged until something clears it,
//! and a device whose slots are both flagged has no path back into
//! Android.
//!
//! This patch makes the flag inert **and erases it on disk**, without
//! touching the retry counter or the slot-switch behaviour:
//!
//! 1. **Setter** — `ORR W9, W9, #0x800000` (the only place that sets
//!    bit 23) becomes `AND W9, W9, #0xFF7FFFFF`, so ABL clears the bit
//!    instead of setting it. The instruction right after it is
//!    `BL <write slot metadata>` (`0x20CDC`), so the cleared value is
//!    persisted to the GPT immediately.
//! 2. **Serializer** — `0x20CDC`
//!    (`UpdatePartitionAttributes`) is the single function that writes
//!    the slot table back to the primary *and* backup GPT. Every caller
//!    (`SetActiveSlot`, `ClearUnbootable`,
//!    `HandleActiveSlotUnbootable`, the retry-count decrement in
//!    `FindBootableSlot`, `MarkPtnActive`) funnels through it. It
//!    byte-packs each entry's `Attributes`; the byte written to
//!    `entry+0x36` (attribute bits 48..55) comes from
//!    `LDRH W8, [X9, #6]`. That load becomes `AND W8, W8, #0x7F`,
//!    which keeps priority/active/retry/successful and clears only
//!    `unbootable`. Every GPT write therefore stores
//!    `unbootable = no` for every slot, whatever RAM or the previous
//!    on-disk image held.
//!
//! Note this only takes effect when ABL writes the table; a plain boot
//! that finds a healthy slot performs no write at all.
//!
//! 3. **Predicate helper** — the tiny leaf
//!    `MOV X9, #0x40000000000000; LSL X8, X8, #32;
//!     AND X8, X8, #0xC0000000000000; CMP X8, X9; RET`
//!    returns "successful && !unbootable". Its `LSL` is rewritten to
//!    `MOV X8, X9`, which makes the following `AND`+`CMP` always
//!    compare equal: every caller (`IsCurrentSlotBootable`, the
//!    `ClearUnbootable` precondition, and the boot-image slot check)
//!    sees a bootable slot for the whole current boot, before the
//!    first GPT write lands.
//! 4. **Decision read** — `UBFX X24, X27, #0x17, #1` (the unbootable
//!    bit feeding the retry/alternate-slot decision) becomes
//!    `MOV X24, XZR`.
//! 5. **Counting read** — `UBFX W8, W8, #0x17, #1` (the "how many
//!    slots are unbootable" accumulator) becomes `MOV W8, WZR`.
//!
//! All five sites are located by exact instruction words/sequences and
//! must be unique in the image; anything unexpected yields `Miss` so
//! the chainload never ships a half-applied patch. Idempotent: a
//! rewritten site no longer matches its stock encoding and is
//! recognised as already applied.
//!
//! Interaction with patch12: patch12 owns the *reset* exits, patch13
//! owns the *state*. Neither depends on the other's bytes.

use crate::internal::encode::write_instr_u32;
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// `ORR W9, W9, #0x800000` — sets the unbootable bit.
const ARM64_SET_UNBOOTABLE: u32 = 0x3209_0129;
/// `LDP W8, W9, [X20, #0x30]!` — loads the attribute word.
const ARM64_LDP_ATTR: u32 = 0x29C6_2688;
/// `STP W8, W9, [X20]` — stores it back.
const ARM64_STP_ATTR: u32 = 0x2900_2688;

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

/// `AND W9, W9, #0xFF7FFFFF` — clears bit 23 instead of setting it.
const ARM64_CLEAR_UNBOOTABLE: u32 = 0x1208_7929;

/// `LDRH W8, [X9, #6]` — GPT-attribute serializer: bytes 6..7 of the
/// partition entry's 64-bit `Attributes` field, i.e. attribute bits
/// 48..63. The low byte of that halfword is what lands in `entry+0x36`,
/// so it carries priority/active/retry/successful/unbootable.
const ARM64_SERIALIZE_ATTR_HI: u32 = 0x7940_0D28;
/// `AND W8, W8, #0x7F` — keeps halfword bits 0..6 (priority, active,
/// retry, successful) and drops bit 7 (`unbootable`).
///
/// The loaded halfword is already the *upper* half of the attribute
/// dword, so the fields live at bit 0..7 here, not at 16..23. The
/// earlier `UBFX X8, X8, #8, #7` extracted attribute bits 56..62 (always
/// zero), which zeroed the entire byte on every write.
const ARM64_SERIALIZE_ATTR_MASKED: u32 = 0x1200_1908;
/// `STRB W8, [X22, #0x36]` — stores the byte into the GPT entry.
const ARM64_SERIALIZE_STRB: u32 = 0x3900_DAC8;
/// `LDRB W8, [X9, #7]` — the next field (attribute bits 56..63). Only
/// used to keep the site unique: `AND W8, W8, #0x7F` on its own is far
/// too common to be an idempotency anchor.
const ARM64_SERIALIZE_TAIL: u32 = 0x3940_1D28;

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
    /// `ORR` that sets the unbootable bit.
    setter_off: u32,
    setter_done: bool,
    /// `LDRH` in the misc serializer.
    serializer_off: u32,
    serializer_done: bool,
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
    // 1. Setter — require the surrounding LDP/ORR/STP triple so we
    //    never rewrite an unrelated `ORR` in a future build.
    let setter_stock = [ARM64_LDP_ATTR, ARM64_SET_UNBOOTABLE, ARM64_STP_ATTR];
    let setter_patched = [ARM64_LDP_ATTR, ARM64_CLEAR_UNBOOTABLE, ARM64_STP_ATTR];
    let (setter_off, setter_done) = locate_site(active, &setter_stock, &setter_patched, 1)?;

    // 2. GPT attribute serializer — the swap must be anchored on its
    //    neighbours; the replacement word alone is not unique.
    let (serializer_off, serializer_done) = locate_site(
        active,
        &[
            ARM64_SERIALIZE_ATTR_HI,
            ARM64_SERIALIZE_STRB,
            ARM64_SERIALIZE_TAIL,
        ],
        &[
            ARM64_SERIALIZE_ATTR_MASKED,
            ARM64_SERIALIZE_STRB,
            ARM64_SERIALIZE_TAIL,
        ],
        0,
    )?;

    // 2. Predicate leaf — 5-word block, only the LSL changes.
    let mut predicate_patched = PREDICATE;
    predicate_patched[PREDICATE_LSL_IDX] = ARM64_MOV_X8_X9;
    let (predicate_off, predicate_done) =
        locate_site(active, &PREDICATE, &predicate_patched, PREDICATE_LSL_IDX)?;

    // 3. Decision read — paired with the adjacent "successful" read so
    //    the replacement (`MOV X24, XZR`) need not be unique on its own.
    let decision_stock = [ARM64_UBFX_DECISION, ARM64_UBFX_SUCCESS];
    let decision_patched = [ARM64_MOV_X24_XZR, ARM64_UBFX_SUCCESS];
    let (decision_off, decision_done) = locate_site(active, &decision_stock, &decision_patched, 0)?;

    // 4. Counter read — paired with the loop's slot-table advance.
    let count_stock = [ARM64_ADVANCE_SLOT, ARM64_UBFX_COUNT];
    let count_patched = [ARM64_ADVANCE_SLOT, ARM64_MOV_W8_WZR];
    let (count_off, count_done) = locate_site(active, &count_stock, &count_patched, 1)?;

    Ok(Plan {
        setter_off,
        setter_done,
        serializer_off,
        serializer_done,
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

    if !p.setter_done {
        write_instr_u32(active, p.setter_off, ARM64_CLEAR_UNBOOTABLE);
    }
    if !p.serializer_done {
        write_instr_u32(active, p.serializer_off, ARM64_SERIALIZE_ATTR_MASKED);
    }
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

    #[test]
    fn set_unbootable_word_matches_encoding() {
        // ORR W9, W9, #0x800000
        assert_eq!(ARM64_SET_UNBOOTABLE, 0x3209_0129);
    }

    #[test]
    fn read_back_after_apply_is_idempotent() {
        // Build a synthetic image holding every stock site.
        let mut img = vec![0u8; 0x400];
        let put = |img: &mut Vec<u8>, off: usize, w: u32| {
            img[off..off + 4].copy_from_slice(&w.to_le_bytes());
        };
        put(&mut img, 0x00, ARM64_LDP_ATTR);
        put(&mut img, 0x04, ARM64_SET_UNBOOTABLE);
        put(&mut img, 0x08, ARM64_STP_ATTR);
        for (i, w) in PREDICATE.iter().enumerate() {
            put(&mut img, 0x40 + i * 4, *w);
        }
        put(&mut img, 0x80, ARM64_SERIALIZE_ATTR_HI);
        put(&mut img, 0x84, ARM64_SERIALIZE_STRB);
        put(&mut img, 0x88, ARM64_SERIALIZE_TAIL);
        put(&mut img, 0x100, ARM64_UBFX_DECISION);
        put(&mut img, 0x104, ARM64_UBFX_SUCCESS);
        put(&mut img, 0x200, ARM64_ADVANCE_SLOT);
        put(&mut img, 0x204, ARM64_UBFX_COUNT);

        let size = img.len() as u32;
        assert_eq!(apply(&mut img, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&img, 0x04), ARM64_CLEAR_UNBOOTABLE);
        assert_eq!(read_instr_u32(&img, 0x80), ARM64_SERIALIZE_ATTR_MASKED);
        assert_eq!(read_instr_u32(&img, 0x44), ARM64_MOV_X8_X9);
        assert_eq!(read_instr_u32(&img, 0x100), ARM64_MOV_X24_XZR);
        assert_eq!(read_instr_u32(&img, 0x204), ARM64_MOV_W8_WZR);

        let snapshot = img.clone();
        assert_eq!(apply(&mut img, size), PatchOutcome::Ok);
        assert_eq!(img, snapshot, "patch13 must be idempotent");
    }

    /// The serializer must keep every field except `unbootable`.
    /// Regression: `UBFX X8, X8, #8, #7` extracted attribute bits 56..62
    /// (always zero) and zeroed the whole byte on every GPT write.
    #[test]
    fn serializer_only_drops_the_unbootable_bit() {
        // Shipped form: AND W8, W8, #0x7F.
        assert_eq!(ARM64_SERIALIZE_ATTR_MASKED, 0x1200_1908);

        // entry+0x36 holds attribute bits 48..63; its low byte packs
        // priority(0..1) active(2) retry(3..5) successful(6) unbootable(7).
        let pack = |prio: u32, active: u32, retry: u32, succ: u32, unb: u32| -> u32 {
            (prio & 3)
                | ((active & 1) << 2)
                | ((retry & 7) << 3)
                | ((succ & 1) << 6)
                | ((unb & 1) << 7)
        };
        let masked = |v: u32| v & 0x7F; // what AND #0x7F produces

        // slot a as it really looks: prio 3, active 1, retry 7, successful 1
        let a = pack(3, 1, 7, 1, 0);
        assert_eq!(masked(a), a, "healthy slot must round-trip unchanged");

        // slot b with unbootable set: only bit 7 disappears
        let b = pack(0, 0, 7, 0, 1);
        assert_eq!(masked(b), pack(0, 0, 7, 0, 0));

        // the old, broken form zeroed everything
        let broken = (b >> 8) & 0x7F;
        assert_eq!(broken, 0);
        assert_ne!(broken, masked(b));
    }

    #[test]
    fn missing_sites_are_a_clean_miss() {
        let mut img = vec![0u8; 0x400];
        let size = img.len() as u32;
        assert_eq!(apply(&mut img, size), PatchOutcome::Miss);
    }
}
