//! patch6 — lock-state fastboot-gate.
//!
//! Port of
//! `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/fastboot_lock_gates.c`.
//!
//! ABL-permissive fakelocks the VerifiedBoot view; ABL's in-fastboot command
//! dispatcher then refuses flash / erase / slot-change / snapshot-cancel.
//! Each refusal site is located through a command prefix plus the
//! `Lock State` discriminator, so wording and line-ending changes in a future
//! ABL do not silently drop a gate:
//!
//! - **Pattern B** — a `B.NE` immediately before a validated refusal entry
//!   jumps PAST the error block. Rewrite it to an unconditional `B` with the
//!   same target.
//! - **Pattern A** — a nearby conditional branch jumps INTO a validated
//!   refusal entry. NOP it.
//!
//! Pattern A accepts `B.cond`, `CBZ/CBNZ` and `TBZ/TBNZ`, because older ABL
//! snapshots use the test-bit forms. The branch search is bounded to the
//! 0x1000 bytes before the first validated entry, and a refusal entry is only
//! accepted after walking back over complete `ADRP+ADD` argument pairs.
//!
//! Every present gate is planned before any byte is written. A build where
//! one gate is understood and another is not is left untouched instead of
//! becoming half-permissive (flash accepted, erase still refused).
//!
//! See `docs/project/re-findings.md`.

use crate::internal::arm64_decode::{decode_adrp_add, decode_branch, Arm64InsnKind};
use crate::internal::encode::{encode_b_uncond, read_instr_u32, write_instr_u32};
use crate::internal::pe_sections::is_pe_file_offset_in_executable_section;
use crate::PatchOutcome;

/// Command prefixes. The complete message must also contain
/// [`LOCK_STATE_MARKER`], so unrelated `… merging state` refusals never match.
const GATE_COMMANDS: &[&[u8]] = &[b"Flashing", b"Erase", b"Slot Change", b"Snapshot Cancel"];

/// Semantic discriminator shared by all four lock-state refusals.
const LOCK_STATE_MARKER: &[u8] = b"Lock State";

/// Search window before the first refusal entry, matching the C reference.
const MAX_GATE_SPAN: u32 = 0x1000;

/// Upper bound on the anchored message (including the NUL terminator).
const MAX_GATE_TEXT: usize = 128;

/// A refusal can load a format string plus the anchored message; walk back at
/// most three complete ADRP+ADD pairs.
const REFUSAL_ENTRY_COUNT: usize = 3;

/// AArch64 NOP.
const ARM64_NOP: u32 = 0xD503_201F;

/// `B.NE` condition code.
const ARM64_COND_NE: u32 = 0x1;

#[inline]
fn contains(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

/// True when `target` starts a NUL-terminated string that begins with
/// `command` and also contains `Lock State` after the prefix.
fn gate_text_at(buf: &[u8], target: u32, command: &[u8]) -> bool {
    let start = target as usize;
    if start >= buf.len() {
        return false;
    }
    let available = core::cmp::min(MAX_GATE_TEXT, buf.len() - start);
    let Some(length) = buf[start..start + available].iter().position(|&b| b == 0) else {
        return false;
    };
    let text = &buf[start..start + length];
    if text.len() < command.len() || &text[..command.len()] != command {
        return false;
    }
    contains(&text[command.len()..], LOCK_STATE_MARKER)
}

/// Count the ADRP+ADD xrefs to a message for `command` and return the first.
fn find_gate_anchor(buf: &[u8], command: &[u8]) -> (u32, u32) {
    let mut found = 0u32;
    let mut first = 0u32;
    let mut off = 0u32;
    while (off as usize) + 8 <= buf.len() {
        if !is_pe_file_offset_in_executable_section(buf, off, 8) {
            off += 4;
            continue;
        }
        if let Some(target) = decode_adrp_add(buf, off) {
            if gate_text_at(buf, target, command) {
                if found == 0 {
                    first = off;
                }
                found += 1;
            }
        }
        off += 4;
    }
    (found, first)
}

/// Decode a conditional branch, including the `TBZ/TBNZ` family which the
/// shared branch decoder does not expose.
fn conditional_branch_target(word: u32, off: u32) -> Option<u32> {
    if let Some(decoded) = decode_branch(word, off) {
        if matches!(
            decoded.kind,
            Arm64InsnKind::CbzW
                | Arm64InsnKind::CbnzW
                | Arm64InsnKind::CbzX
                | Arm64InsnKind::CbnzX
                | Arm64InsnKind::Bcond
        ) {
            return Some(decoded.target_off);
        }
    }
    // TBZ/TBNZ: b5:op:011011:imm14:Rt. The op bit is masked out so both
    // directions share one decoder.
    if (word & 0x7E00_0000) == 0x3600_0000 {
        let imm14 = (word >> 5) & 0x3FFF;
        let signed = if (imm14 & 0x2000) != 0 {
            (imm14 | 0xFFFF_C000) as i32
        } else {
            imm14 as i32
        };
        return Some((off as i32).wrapping_add(signed * 4) as u32);
    }
    None
}

/// Walk back over complete ADRP+ADD argument pairs preceding `anchor`.
fn refusal_entries(buf: &[u8], anchor: u32) -> ([u32; REFUSAL_ENTRY_COUNT], usize) {
    let mut entries = [0u32; REFUSAL_ENTRY_COUNT];
    entries[0] = anchor;
    let mut count = 1usize;
    while count < REFUSAL_ENTRY_COUNT {
        let entry = entries[count - 1];
        if entry < 8 {
            break;
        }
        match decode_adrp_add(buf, entry - 8) {
            Some(target) if (target as usize) < buf.len() => {
                entries[count] = entry - 8;
                count += 1;
            }
            _ => break,
        }
    }
    (entries, count)
}

/// Plan one gate. `Ok(None)` means this build does not ship the gate;
/// `Ok(Some((offset, word)))` is the single rewrite to apply.
fn plan_one_gate(buf: &[u8], command: &[u8]) -> Result<Option<(u32, u32)>, PatchOutcome> {
    let (anchors, anchor) = find_gate_anchor(buf, command);
    if anchors == 0 {
        return Ok(None);
    }
    if anchors > 1 {
        return Err(PatchOutcome::Ambiguous);
    }
    let (entries, count) = refusal_entries(buf, anchor);

    // Pattern B: B.NE immediately before a validated refusal entry, skipping
    // past the anchored block. Require the target to land just beyond it.
    let mut direct_skip: Option<(u32, u32)> = None;
    let mut direct_skips = 0u32;
    for &entry in &entries[..count] {
        if entry < 4 {
            continue;
        }
        let at = entry - 4;
        let word = read_instr_u32(buf, at);
        let Some(decoded) = decode_branch(word, at) else {
            continue;
        };
        if decoded.kind != Arm64InsnKind::Bcond || decoded.reg_or_cond != ARM64_COND_NE {
            continue;
        }
        let target = decoded.target_off;
        if target < anchor.saturating_add(8)
            || target > anchor.saturating_add(MAX_GATE_SPAN)
            || (target as usize) >= buf.len()
        {
            continue;
        }
        direct_skips += 1;
        direct_skip = Some((at, target));
    }
    if direct_skips > 1 {
        return Err(PatchOutcome::Ambiguous);
    }
    if let Some((at, target)) = direct_skip {
        return match encode_b_uncond(at, target) {
            Some(word) => Ok(Some((at, word))),
            None => Err(PatchOutcome::Miss),
        };
    }

    // Pattern A: one nearby conditional edge enters the validated prefix.
    let first_entry = entries[count - 1];
    let start = first_entry.saturating_sub(MAX_GATE_SPAN);
    let mut branch: Option<u32> = None;
    let mut branches = 0u32;
    let mut off = start;
    while off < first_entry {
        if (off as usize) + 4 > buf.len() {
            break;
        }
        if is_pe_file_offset_in_executable_section(buf, off, 4) {
            if let Some(target) = conditional_branch_target(read_instr_u32(buf, off), off) {
                if entries[..count].contains(&target) {
                    branches += 1;
                    branch = Some(off);
                }
            }
        }
        off += 4;
    }
    match (branches, branch) {
        (0, _) => Err(PatchOutcome::Miss),
        (1, Some(off)) => Ok(Some((off, ARM64_NOP))),
        _ => Err(PatchOutcome::Ambiguous),
    }
}

/// patch6 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];

    // Plan every gate first. The C reference refuses to touch a build where
    // one gate is understood and another is not: a half-patched dispatcher
    // would accept flash but still refuse erase.
    let mut offsets = [0u32; GATE_COMMANDS.len()];
    let mut words = [0u32; GATE_COMMANDS.len()];
    let mut planned = 0usize;
    for command in GATE_COMMANDS {
        match plan_one_gate(active, command) {
            Ok(Some((off, word))) => {
                offsets[planned] = off;
                words[planned] = word;
                planned += 1;
            }
            Ok(None) => {}
            Err(outcome) => return outcome,
        }
    }

    if planned == 0 {
        // No supported refusal strings present — not an oplus/oppo ABL.
        return PatchOutcome::Miss;
    }

    for index in 0..planned {
        write_instr_u32(active, offsets[index], words[index]);
    }
    PatchOutcome::Ok
}

#[cfg(all(test, feature = "std"))]
mod tests {
    use super::*;
    use crate::internal::encode::encode_cbz;

    const PE_OFF: usize = 0x40;
    const SECTION_OFF: usize = PE_OFF + 24 + 0xE0;
    const TEXT_PTR: u32 = 0x400;
    const TEXT_SIZE: u32 = 0x3000;

    fn put_u16(buf: &mut [u8], off: usize, value: u16) {
        buf[off..off + 2].copy_from_slice(&value.to_le_bytes());
    }
    fn put_u32(buf: &mut [u8], off: usize, value: u32) {
        buf[off..off + 4].copy_from_slice(&value.to_le_bytes());
    }

    /// Minimal PE32+ with one executable `.text` section whose raw offset and
    /// VA are equal, so file offsets and RVAs coincide in the test.
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
        buf
    }

    fn encode_adrp(rd: u32, insn_off: u32, target: u32) -> u32 {
        let delta = ((target as i32) - ((insn_off & !0xFFF) as i32)) >> 12;
        let imm21 = (delta as u32) & 0x1F_FFFF;
        0x9000_0000 | ((imm21 & 3) << 29) | (((imm21 >> 2) & 0x7_FFFF) << 5) | rd
    }

    fn encode_add_x(rd: u32, rn: u32, imm12: u32) -> u32 {
        0x9100_0000 | ((imm12 & 0xFFF) << 10) | (rn << 5) | rd
    }

    fn place_adrp_add(buf: &mut [u8], off: u32, target: u32, reg: u32) {
        let adrp = encode_adrp(reg, off, target & !0xFFF);
        let add = encode_add_x(reg, reg, target & 0xFFF);
        put_u32(buf, off as usize, adrp);
        put_u32(buf, off as usize + 4, add);
    }

    fn place_string(buf: &mut [u8], off: u32, text: &[u8]) {
        let start = off as usize;
        buf[start..start + text.len()].copy_from_slice(text);
        buf[start + text.len()] = 0;
    }

    fn encode_bcond(insn_off: u32, target: u32, cond: u32) -> u32 {
        let delta = ((target as i32) - (insn_off as i32)) / 4;
        let imm19 = (delta as u32) & 0x7_FFFF;
        0x5400_0000 | (imm19 << 5) | cond
    }

    fn encode_tbz(insn_off: u32, target: u32, reg: u32, bit: u32) -> u32 {
        let delta = ((target as i32) - (insn_off as i32)) / 4;
        let imm14 = (delta as u32) & 0x3FFF;
        0x3600_0000 | ((bit & 0x1F) << 19) | (imm14 << 5) | reg
    }

    #[test]
    fn prefix_match_accepts_reworded_message() {
        let mut buf = synthetic_pe();
        place_string(&mut buf, 0x2000, b"Flashing denied while in Lock State");
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        put_u32(&mut buf, 0xFFC, encode_cbz(0xFFC, 0x1000, 0).unwrap());

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 0xFFC), ARM64_NOP);
    }

    #[test]
    fn tbz_gate_is_noped() {
        let mut buf = synthetic_pe();
        place_string(&mut buf, 0x2000, b"Erase is not allowed in Lock State");
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        put_u32(&mut buf, 0xFFC, encode_tbz(0xFFC, 0x1000, 0, 0));

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 0xFFC), ARM64_NOP);
    }

    #[test]
    fn pattern_b_becomes_unconditional_branch() {
        let mut buf = synthetic_pe();
        place_string(
            &mut buf,
            0x2000,
            b"Slot Change is not allowed in Lock State\n",
        );
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        let target = 0x1040u32;
        put_u32(&mut buf, 0xFFC, encode_bcond(0xFFC, target, ARM64_COND_NE));

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        let decoded = decode_branch(read_instr_u32(&buf, 0xFFC), 0xFFC).unwrap();
        assert_eq!(decoded.kind, Arm64InsnKind::B);
        assert_eq!(decoded.target_off, target);
    }

    #[test]
    fn ambiguous_anchor_writes_nothing() {
        let mut buf = synthetic_pe();
        place_string(&mut buf, 0x2000, b"Flashing is not allowed in Lock State");
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        place_adrp_add(&mut buf, 0x1010, 0x2000, 1);
        put_u32(&mut buf, 0xFFC, encode_cbz(0xFFC, 0x1000, 0).unwrap());
        let before = buf.clone();

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ambiguous);
        assert_eq!(buf, before, "ambiguous plan must not write any gate");
    }

    #[test]
    fn unresolvable_second_gate_blocks_the_first() {
        let mut buf = synthetic_pe();
        place_string(&mut buf, 0x2000, b"Flashing is not allowed in Lock State");
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        let flashing_branch = encode_cbz(0xFFC, 0x1000, 0).unwrap();
        put_u32(&mut buf, 0xFFC, flashing_branch);

        // Erase has a valid string and xref but no branch into its entry.
        place_string(&mut buf, 0x2100, b"Erase is not allowed in Lock State");
        place_adrp_add(&mut buf, 0x1100, 0x2100, 0);
        let before = buf.clone();

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
        assert_eq!(buf, before, "a failed gate must block every rewrite");
        assert_eq!(read_instr_u32(&buf, 0xFFC), flashing_branch);
    }

    #[test]
    fn merge_state_refusal_does_not_match() {
        let mut buf = synthetic_pe();
        place_string(
            &mut buf,
            0x2000,
            b"Slot Change is not allowed in merging state",
        );
        place_adrp_add(&mut buf, 0x1000, 0x2000, 0);
        put_u32(&mut buf, 0xFFC, encode_cbz(0xFFC, 0x1000, 0).unwrap());
        let before = buf.clone();

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
        assert_eq!(buf, before);
    }

    #[test]
    fn tbz_into_prefixed_refusal_block_is_noped() {
        let mut buf = synthetic_pe();
        // The refusal prints a format string first; the anchored message is
        // loaded by the second ADRP+ADD. The gate branches to the first entry.
        place_string(&mut buf, 0x2200, b"ERROR: %a");
        place_string(
            &mut buf,
            0x2000,
            b"Snapshot Cancel denied while in Lock State.",
        );
        place_adrp_add(&mut buf, 0xFF0, 0x2200, 0);
        place_adrp_add(&mut buf, 0xFF8, 0x2000, 1);
        put_u32(&mut buf, 0xFC0, encode_tbz(0xFC0, 0xFF0, 0, 0));

        let size = buf.len() as u32;
        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        assert_eq!(read_instr_u32(&buf, 0xFC0), ARM64_NOP);
    }

    #[test]
    fn tracked_infiniti_fixture_resolves_all_four_gates() {
        let path = std::path::Path::new(env!("CARGO_MANIFEST_DIR"))
            .join("../../tests/images/pe/infiniti-EU-16.0.5.703.efi");
        let Ok(data) = std::fs::read(&path) else {
            return;
        };
        let size = data.len() as u32;
        let mut buf = data.clone();

        assert_eq!(apply(&mut buf, size), PatchOutcome::Ok);
        // Flashing / Erase use Pattern A (CBZ into the refusal block).
        assert_eq!(read_instr_u32(&buf, 0x1C3EC), ARM64_NOP);
        assert_eq!(read_instr_u32(&buf, 0x1DF70), ARM64_NOP);
        // Slot Change / Snapshot Cancel use Pattern B (B.NE past the block).
        assert_eq!(read_instr_u32(&buf, 0x1AA5C) & 0x7C00_0000, 0x1400_0000);
        assert_eq!(read_instr_u32(&buf, 0x1C1F4) & 0x7C00_0000, 0x1400_0000);

        // Refusal strings stay intact, and a second pass has no gate left.
        for command in GATE_COMMANDS {
            assert!(buf.windows(command.len()).any(|w| w == *command));
        }
        assert_eq!(apply(&mut buf, size), PatchOutcome::Miss);
    }
}
