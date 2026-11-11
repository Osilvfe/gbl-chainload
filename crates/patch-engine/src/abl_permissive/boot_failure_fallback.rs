//! patch12 — boot-failure fastboot fallback.
//!
//! The vendor boot flow keeps the A/B retry counter and slot switching
//! exactly as designed. What it does *not* do is let a device with no
//! bootable slot reach fastboot when the secure-lock state is set: two
//! failure paths print a message and then cold-reset, so the device
//! resets forever and never exposes fastboot (the "orange branch" from
//! the A/B flow chart).
//!
//! Both sites share the same shape:
//!
//! ```text
//!   ADRP/ADD X1, "<anchor string>"
//!   MOV      W0, #0x80000000    ; debug level
//!   BL       <print>
//!   MOV      W0, #0x3E          ; reset reason
//!   BL       <gRT->ResetSystem> ; <-- rewritten to NOP
//!   ...                         ; fall through to fastboot
//! ```
//!
//! The two anchors are:
//!
//! - `"if olock secure lock state, Not allow Fastboot.\n"` — reached when
//!   the boot attempt failed while the OPPO secure-lock state is set.
//!   Falls through to the "No fastboot support" check and then
//!   `Launching fastboot`.
//! - `"fastboot_unlock_verify error and reboot."` — the fastboot
//!   allow-list check refused the boot. Falls through to the fastboot
//!   build banner and `fastboot` itself.
//!
//! The retry counter is never touched: exhaustion still marks a slot
//! unbootable and still switches to the alternate slot. Only the final
//! "no slot left -> reset" exit is redirected.
//!
//! This is a host-side ABL patch (rides `CACHED_ABL`).

use crate::internal::arm64_decode::find_adrp_add_targeting;
use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// Reached when the boot attempt failed while olock secure-lock is set.
const ANCHOR_LOCKED_FASTBOOT: &[u8] = b"if olock secure lock state, Not allow Fastboot.";
/// Reached when the fastboot allow-list check refuses the boot.
const ANCHOR_UNLOCK_VERIFY: &[u8] = b"fastboot_unlock_verify error and reboot";

/// AArch64 `NOP`.
const ARM64_NOP: u32 = 0xD503_201F;

/// `MOV W0, #0x3E` — the cold-reset reason passed to the reset helper.
const ARM64_MOV_W0_RESET_REASON: u32 = 0x5280_07C0;

/// `BL imm26`.
const ARM64_BL_MASK: u32 = 0xFC00_0000;
const ARM64_BL_PAT: u32 = 0x9400_0000;

/// Offsets from the anchor's ADRP to the `MOV W0, #0x3E` / `BL <reset>`.
const OFF_RESET_REASON: u32 = 0x10;
const OFF_RESET_CALL: u32 = 0x14;

/// One located reset site.
struct Site {
    /// File offset of the `BL <reset>` instruction.
    call_off: u32,
    /// Resolved BL target, absent when the site is already patched.
    target: Option<u32>,
    /// `BL` already rewritten to `NOP`.
    already: bool,
}

/// Decode a `BL` target from `pc` and the instruction word.
fn bl_target(pc: u32, insn: u32) -> u32 {
    // imm26 is a signed instruction delta.
    let imm = ((insn & 0x03FF_FFFF) << 6) as i32 >> 6;
    (pc as i32).wrapping_add(imm.wrapping_mul(4)) as u32
}

/// Locate one reset site via its unique log string.
fn locate(active: &[u8], anchor: &[u8]) -> Result<Site, PatchOutcome> {
    let (r, str_off) = scan_for(active, anchor, None);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return Err(PatchOutcome::Ambiguous),
        _ => return Err(PatchOutcome::Miss),
    }

    let (r, adrp_off) = find_adrp_add_targeting(active, str_off, true);
    match r {
        ScanResult::Found => {}
        ScanResult::Ambiguous => return Err(PatchOutcome::Ambiguous),
        _ => return Err(PatchOutcome::Miss),
    }

    let reason_off = adrp_off.saturating_add(OFF_RESET_REASON);
    let call_off = adrp_off.saturating_add(OFF_RESET_CALL);
    if (call_off as usize) + 4 > active.len() {
        return Err(PatchOutcome::Miss);
    }

    // The reset reason must be exactly 0x3E; anything else means the
    // surrounding code drifted and we refuse to touch it.
    if read_instr_u32(active, reason_off) != ARM64_MOV_W0_RESET_REASON {
        return Err(PatchOutcome::Miss);
    }

    let word = read_instr_u32(active, call_off);
    if word == ARM64_NOP {
        return Ok(Site {
            call_off,
            target: None,
            already: true,
        });
    }
    if (word & ARM64_BL_MASK) != ARM64_BL_PAT {
        return Err(PatchOutcome::Miss);
    }

    Ok(Site {
        call_off,
        target: Some(bl_target(call_off, word)),
        already: false,
    })
}

/// patch12 apply function.
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];

    let locked = match locate(active, ANCHOR_LOCKED_FASTBOOT) {
        Ok(s) => s,
        Err(e) => return e,
    };
    let verify = match locate(active, ANCHOR_UNLOCK_VERIFY) {
        Ok(s) => s,
        Err(e) => return e,
    };

    // Both exits must route through the same reset helper. If they do
    // not, this is a build whose failure path we do not understand.
    if let (Some(a), Some(b)) = (locked.target, verify.target) {
        if a != b {
            return PatchOutcome::Ambiguous;
        }
    }

    if !locked.already {
        write_instr_u32(active, locked.call_off, ARM64_NOP);
    }
    if !verify.already {
        write_instr_u32(active, verify.call_off, ARM64_NOP);
    }

    PatchOutcome::Ok
}
