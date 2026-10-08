//! patch11 — bootconfig locked-handoff ("lifecycle split" spoof).
//!
//! Goal (方案1 from the 2026-10-08 session): ABL keeps its real
//! unlocked internal state during image load / AVB verification (so
//! patch10's permissive path, TWRP, and self-signed vbmeta keep
//! working), but every outbound *handoff* surface tells the OS it is
//! locked with a green verified-boot state. This is the bootloader-side
//! replacement for userspace property spoofing (no KernelSU/Zygisk
//! needed for the `ro.boot.*` layer).
//!
//! On GBL-era Qcom ABLs (OnePlus 15/15T, Xiaomi 17 verified; no
//! `androidboot.flash.locked` literal exists anymore — userspace derives
//! it from `androidboot.vbmeta.device_state`) the state reaches the OS
//! through exactly two producers:
//!
//! 1. `androidboot.vbmeta.device_state` — assembled by a
//!    `cmp wN, #0; csel x2, xLocked, xUnlocked, eq` pair and appended to
//!    the bootconfig list. The CSEL is **byte-identical across all five
//!    inspected builds** (`0x9A890142`).
//! 2. `androidboot.verifiedbootstate` — every consumer (bootconfig
//!    entry, cmdline mirror, ` oplusboot.verifiedbootstate=` mirror,
//!    debug prints) resolves the color name through one 16-byte-stride
//!    `.data` table: `{0:"green", 1:"orange", 2:"yellow", 3:"red"}`.
//!
//! The patch therefore has two halves:
//!
//! - **11a — device_state CSEL.** Anchor on the unique
//!   `androidboot.vbmeta.device_state` string, resolve its unique
//!   ADRP+ADD xref, find the following CSEL, validate that its EQ
//!   operand resolves to `"locked"` and its other operand to
//!   `"unlocked"`, then rewrite `CSEL Xd, Xn, Xm, eq` → `MOV Xd, Xn`
//!   (always hand out `"locked"`). Idempotent: a rewritten site no
//!   longer matches CSEL; the already-applied check recognizes the
//!   `MOV Xd, Xn` whose source operand resolves to `"locked"`.
//! - **11b — color table redirect.** Locate the unique
//!   `{0..3} × {"green","orange","yellow","red"}` table in a
//!   non-executable PE section and repoint the orange and yellow slots
//!   at the green string. `red` is left intact (it only occurs on
//!   verification failure, which patch10 already forces to success).
//!   Idempotent: slots already equal to green satisfy the match.
//!
//! Both halves must land (or already be applied) for `PATCH_OK`; any
//! anchor failure is a `MISS` (optional patch — never blocks the
//! chainload). Multiple table candidates degrade to `AMBIGUOUS`.
//!
//! See `docs/project/re-findings.md` § patch11.

use crate::internal::arm64_decode::{decode_adrp_add_regs, find_adrp_add_targeting};
use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// Unique bootconfig key string — the sole producer anchor for 11a.
const PATCH11_ANCHOR: &[u8] = b"androidboot.vbmeta.device_state";

/// Value strings handed to the CSEL. Matched NUL-terminated so the
/// operand check cannot accidentally accept `"...unlockable"`-style
/// neighbors.
const STR_LOCKED: &[u8] = b"locked\0";
const STR_UNLOCKED: &[u8] = b"unlocked\0";

/// Color-table strings for 11b.
const STR_GREEN: &[u8] = b"green\0";
const STR_ORANGE: &[u8] = b"orange\0";
const STR_YELLOW: &[u8] = b"yellow\0";
const STR_RED: &[u8] = b"red\0";

/// CSEL (64-bit): `sf 0 0 1101 0100 op2:0 S? cond Rm 01 Rn Rd` —
/// fixed bits masked to `0x1A800000` with sf=1.
const ARM64_CSEL64_MASK: u32 = 0x7FE0_0C00;
const ARM64_CSEL64_PAT: u32 = 0x1A80_0000;

/// `MOV Xd, Xn` (alias of `ORR Xd, XZR, Xn`): Rn must be XZR(31).
const ARM64_MOV_REG_MASK: u32 = 0xFFE0_FFE0;
const ARM64_MOV_REG_PAT: u32 = 0xAA00_03E0;

/// How far after the key-string ADRP the CSEL may sit (instructions).
const CSEL_WINDOW: u32 = 8;
/// How far before the CSEL the operand ADRP+ADD pairs may sit (bytes).
const OPERAND_WINDOW: u32 = 0x30;

/// patch11 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let a = apply_device_state_csel(buf, size);
    let b = apply_color_table_redirect(buf, size);
    match (a, b) {
        (PatchOutcome::Ok, PatchOutcome::Ok) => PatchOutcome::Ok,
        (PatchOutcome::Ambiguous, _) | (_, PatchOutcome::Ambiguous) => PatchOutcome::Ambiguous,
        _ => PatchOutcome::Miss,
    }
}

/// Read a NUL-terminated string at `off` and compare with `needle`
/// (needle includes its NUL). `off` is a resolved VA — for ABL PEs
/// image base is 0 and section VA == file offset, the same identity
/// assumption the other string-anchored patches rely on.
fn cstr_eq(buf: &[u8], off: u32, needle: &[u8]) -> bool {
    let off_u = off as usize;
    if off_u + needle.len() > buf.len() {
        return false;
    }
    &buf[off_u..off_u + needle.len()] == needle
}

/// Find the nearest ADRP+ADD pair before `before_off` (within
/// [`OPERAND_WINDOW`]) that writes `reg`, and return whether the
/// materialized address holds `needle`.
fn reg_resolves_to(buf: &[u8], reg: u32, before_off: u32, needle: &[u8]) -> bool {
    let floor = before_off.saturating_sub(OPERAND_WINDOW);
    let mut probe = before_off;
    while probe >= floor + 4 && probe >= 4 {
        probe -= 4;
        if let Some((addr, rd)) = decode_adrp_add_regs(buf, probe) {
            if rd == reg && cstr_eq(buf, addr, needle) {
                return true;
            }
        }
    }
    false
}

/// 11a — force `androidboot.vbmeta.device_state` to always hand out
/// `"locked"`.
fn apply_device_state_csel(buf: &mut [u8], _size: u32) -> PatchOutcome {
    // 1. Unique anchor string.
    let (r, str_off) = scan_for(buf, PATCH11_ANCHOR, None);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 2. Unique ADRP+ADD xref in executable code.
    let (r, adrp_off) = find_adrp_add_targeting(buf, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return PatchOutcome::Ambiguous,
        _ => return PatchOutcome::Miss,
    }

    // 3. Forward window: find the CSEL, or recognize an already-applied
    //    MOV whose source operand resolves to "locked".
    let mut csel_off: u32 = 0;
    let mut already = false;
    let limit = adrp_off.saturating_add(CSEL_WINDOW * 4);
    let mut p = adrp_off + 4;
    while p + 4 <= limit.min(buf.len() as u32) {
        let word = read_instr_u32(buf, p);
        if (word & ARM64_CSEL64_MASK) == ARM64_CSEL64_PAT {
            csel_off = p;
            break;
        }
        if (word & ARM64_MOV_REG_MASK) == ARM64_MOV_REG_PAT {
            let rn = (word >> 16) & 0x1F;
            if reg_resolves_to(buf, rn, p, STR_LOCKED) {
                already = true;
                break;
            }
        }
        p += 4;
    }
    if already {
        return PatchOutcome::Ok;
    }
    if csel_off == 0 {
        return PatchOutcome::Miss;
    }

    // 4. Validate the CSEL: EQ condition, EQ-operand resolves to
    //    "locked", other operand resolves to "unlocked". Anything else
    //    (inverted sense, different strings) is a build we do not
    //    understand — refuse to anchor.
    let word = read_instr_u32(buf, csel_off);
    let cond = (word >> 12) & 0xF;
    let rd = word & 0x1F;
    let rn = (word >> 5) & 0x1F;
    let rm = (word >> 16) & 0x1F;
    if cond != 0
        || !reg_resolves_to(buf, rn, csel_off, STR_LOCKED)
        || !reg_resolves_to(buf, rm, csel_off, STR_UNLOCKED)
    {
        return PatchOutcome::Miss;
    }

    // 5. Rewrite CSEL -> MOV Rd, Rn (always the "locked" operand).
    let mov_insn = ARM64_MOV_REG_PAT | (rn << 16) | rd;
    write_instr_u32(buf, csel_off, mov_insn);
    PatchOutcome::Ok
}

/// Iterate the PE section table, invoking `f(raw_start, raw_len,
/// characteristics)` for every section with raw data.
fn for_each_section<F: FnMut(u32, u32, u32)>(buf: &[u8], mut f: F) {
    let size = buf.len();
    if size < 0x100 || buf[0] != b'M' || buf[1] != b'Z' {
        return;
    }
    let rd32 = |o: usize| -> u32 {
        (buf[o] as u32) | ((buf[o + 1] as u32) << 8) | ((buf[o + 2] as u32) << 16)
            | ((buf[o + 3] as u32) << 24)
    };
    let pe_off = rd32(0x3C) as usize;
    if pe_off > size || pe_off + 0x18 > size || &buf[pe_off..pe_off + 4] != b"PE\0\0" {
        return;
    }
    let nsec = rd32(pe_off + 6) & 0xFFFF;
    let opt_size = (rd32(pe_off + 20) & 0xFFFF) as usize;
    let sec_off = pe_off + 24 + opt_size;
    for i in 0..(nsec as usize) {
        let sh = sec_off + i * 40;
        if sh + 40 > size {
            return;
        }
        let raw_size = rd32(sh + 16);
        let raw_ptr = rd32(sh + 20);
        let chars = rd32(sh + 36);
        if raw_size == 0 {
            continue;
        }
        if (raw_ptr as usize).saturating_add(raw_size as usize) > size {
            continue;
        }
        f(raw_ptr, raw_size, chars);
    }
}

/// Read a little-endian u64 at an unaligned offset, if in bounds.
fn read_u64_le(buf: &[u8], off: usize) -> Option<u64> {
    if off + 8 > buf.len() {
        return None;
    }
    let mut v: u64 = 0;
    for i in 0..8 {
        v |= (buf[off + i] as u64) << (8 * i);
    }
    Some(v)
}

/// Write a little-endian u64 at an offset (bounds already validated by
/// the caller).
fn write_u64_le(buf: &mut [u8], off: usize, v: u64) {
    for i in 0..8 {
        buf[off + i] = (v >> (8 * i)) as u8;
    }
}

/// 11b — repoint the verified-boot color table's orange and yellow
/// slots at the green string, so every `*verifiedbootstate` consumer
/// reports `green`.
fn apply_color_table_redirect(buf: &mut [u8], _size: u32) -> PatchOutcome {
    // Candidate = a 16-byte-stride qword 4-tuple layout
    //   {idx@+0, ptr@+8} x4  with idx 0..3, ptr -> green/orange/yellow/red.
    // The orange/yellow slots may already point at green (idempotent
    // re-apply); red must stay red.
    let mut hits: [u32; 4] = [0; 4];
    let mut sections: [(u32, u32); 4] = [(0, 0); 4];

    for_each_section(buf, |raw_ptr, raw_size, chars| {
        // Skip executable sections: the table lives in data, and this
        // avoids coincidental matches inside code.
        if (chars & 0x2000_0000) != 0 {
            return;
        }
        let start = ((raw_ptr + 7) & !7u32) as usize; // 8-align
        let end = (raw_ptr + raw_size) as usize;
        if start + 64 > end {
            return;
        }
        let mut off = start;
        while off + 64 <= end {
            let idx = |o: usize| read_u64_le(buf, o).map(|v| v as u32);
            let ptr = |o: usize| read_u64_le(buf, o);
            let (i0, i1, i2, i3) = match (
                idx(off),
                idx(off + 16),
                idx(off + 32),
                idx(off + 48),
            ) {
                (Some(a), Some(b), Some(c), Some(d)) => (a, b, c, d),
                _ => break,
            };
            if (i0, i1, i2, i3) == (0, 1, 2, 3) {
                let p_green = ptr(off + 8).unwrap_or(0) as u32;
                let p_orange = ptr(off + 24).unwrap_or(0) as u32;
                let p_yellow = ptr(off + 40).unwrap_or(0) as u32;
                let p_red = ptr(off + 56).unwrap_or(0) as u32;
                let green_ok = cstr_eq(buf, p_green, STR_GREEN);
                let red_ok = cstr_eq(buf, p_red, STR_RED);
                let orange_ok =
                    cstr_eq(buf, p_orange, STR_ORANGE) || cstr_eq(buf, p_orange, STR_GREEN);
                let yellow_ok =
                    cstr_eq(buf, p_yellow, STR_YELLOW) || cstr_eq(buf, p_yellow, STR_GREEN);
                if green_ok && red_ok && orange_ok && yellow_ok {
                    if hits[3] < 4 {
                        // record up to 4 hits; only the first is needed
                        // for the unique-apply, the count decides.
                        sections[hits[3] as usize] = (off as u32, p_green);
                    }
                    hits[3] += 1;
                }
            }
            off += 8;
        }
    });

    let count = hits[3];
    match count {
        0 => return PatchOutcome::Miss,
        1 => {}
        _ => return PatchOutcome::Ambiguous,
    }

    let (off, p_green) = sections[0];
    let off_u = off as usize;
    let p_green = p_green as u64;
    let already =
        read_u64_le(buf, off_u + 24) == Some(p_green) && read_u64_le(buf, off_u + 40) == Some(p_green);
    if !already {
        write_u64_le(buf, off_u + 24, p_green);
        write_u64_le(buf, off_u + 40, p_green);
    }
    PatchOutcome::Ok
}
