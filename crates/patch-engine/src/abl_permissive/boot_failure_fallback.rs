//! patch12 — boot-failure fallback: disable the orange branch (v3).
//!
//! The vendor boot flow keeps the A/B retry counter and slot switching
//! exactly as designed. What it does *not* do is let a device with no
//! bootable slot reach fastboot when the OPPO secure-lock state is set.
//!
//! Decompiled shape of `LinuxLoaderEntry` (fairlady PLZ110 16.0.5.701,
//! PE offsets shown; the same shape is present in the EU 703 build):
//!
//! ```text
//!   v15 = gBS->LocateProtocol (&gEfiMemCardInfoProtocolGuid, 0, &v338);
//!   v8  = v15;                              // 0x5070  x19
//!   if (v15 < 0) { ...; goto LABEL_23; }    // still continues
//!   ...
//!   v162 = (olock_secure_lock_state == 1);  // 0x35660
//!   ...
//!   if (fastboot_requested) {               // byte_100BAB88
//!       if (v162) goto REFUSE;              // 0x6CD8
//! LABEL_341:
//!       if (v8 < 0) goto SPIN;              // 0x6D64
//! LABEL_342:
//!       print "Launching fastboot";         // 0x6D68
//!       if (fastboot_unlock_verify() != 0) {
//!           print "...error and reboot.";
//!           ResetSystem(0x3E);              // 0x6D94
//!       }
//!       ...
//!   }
//!   REFUSE:                                 // 0x6CDC
//!       print "if olock secure lock state, Not allow Fastboot.";
//!       ResetSystem(0x3E);                  // 0x6CF0
//!       if (v8 < 0) goto SPIN;              // 0x6CF4
//!       goto LABEL_342;
//!   SPIN:                                   // 0x6CF8
//!       print "No fastboot support for flashless chipsets, Infinte loop";
//!       for (;;) ;                          // 0x6D08
//! ```
//!
//! Two independent ways to lose the device live here:
//!
//! * **REFUSE** cold-resets, so an A/B-exhausted device resets forever
//!   and never exposes fastboot ("orange branch").
//! * **SPIN** never resets and never enumerates USB, so nothing can be
//!   caught at all. It is reached from LABEL_341 when the MemCardInfo
//!   protocol lookup failed — which the vendor equates with a flashless
//!   chipset, but which also covers a driver that simply was not
//!   connected yet in the boot-failure path.
//!
//! v1 of this patch NOPed both resets, which converted the reset loop
//! into SPIN and bricked a device with no way back. v2 kept the reset
//! for the `v8 < 0` case, which still allows SPIN to be reached through
//! REFUSE. **v3 disables both**: nothing in the fastboot path may reset
//! or spin any more, and control always reaches LABEL_342.
//!
//! Three rewrites, all validated against their exact stock encodings:
//!
//! 1. `0x6CD8` `CBZ W26, 0x6D64` → `B 0x6D68` — an A/B-exhausted device
//!    with the secure-lock state set goes straight to fastboot.
//! 2. `0x6D64` `TBNZ X19, #63, 0x6CF8` → `B 0x6D68` — the shared tail
//!    of the `LABEL_341` test in the non-fastboot path also continues to
//!    fastboot instead of spinning forever.
//! 3. `0x6D94` `BL <ResetSystem>` → `NOP` — the
//!    `fastboot_unlock_verify` refusal falls through to the normal
//!    "Fastboot Build Info" banner instead of rebooting.
//!
//! After (1), the REFUSE block (`0x6CDC`..`0x6CF4`) is unreachable, and
//! after (2) the SPIN block (`0x6CF8`..`0x6D08`) is unreachable, so the
//! device always reaches fastboot. The reset calls themselves are left
//! in place as dead code: this patch removes reachability, not
//! instructions nobody else depends on.
//!
//! The A/B retry counter and slot switching are never touched.

use crate::internal::arm64_decode::{decode_branch, find_adrp_add_targeting, Arm64InsnKind};
use crate::internal::encode::{read_instr_u32, write_instr_u32};
use crate::internal::scan::{scan_for, ScanResult};
use crate::PatchOutcome;

/// Reached when the boot attempt failed while olock secure-lock is set.
const ANCHOR_LOCKED_FASTBOOT: &[u8] = b"if olock secure lock state, Not allow Fastboot.";
/// Reached when the fastboot allow-list check refuses the boot.
const ANCHOR_UNLOCK_VERIFY: &[u8] = b"fastboot_unlock_verify error and reboot";
/// Printed right before the "no fastboot transport" dead spin.
const ANCHOR_NO_SUPPORT: &[u8] = b"No fastboot support for flashless chipsets, Infinte loop";
/// Printed by the second (non-fastboot) entry into the refusal block.
const ANCHOR_INIT_RECORD: &[u8] = b"OplusInitRecordData failed: %r\n";
/// Offsets from that anchor's ADRP to its `CBZ W26, LABEL_341`.
const OFF_INIT_RECORD_GATE: u32 = 0x1C;

/// AArch64 `NOP`.
const ARM64_NOP: u32 = 0xD503_201F;
/// `MOV W0, #0x3E` — the cold-reset reason passed to the reset helper.
const ARM64_MOV_W0_RESET_REASON: u32 = 0x5280_07C0;

/// Offsets from the anchor's ADRP to the `MOV W0, #0x3E` / `BL <reset>`.
const OFF_RESET_REASON: u32 = 0x10;
const OFF_RESET_CALL: u32 = 0x14;
/// The `TBZ Xn, #63, <launch fastboot>` that follows the reset call.
const OFF_STATUS_TEST: u32 = 0x18;

/// `BL imm26`.
const ARM64_BL_MASK: u32 = 0xFC00_0000;
const ARM64_BL_PAT: u32 = 0x9400_0000;

#[inline]
fn sign_extend14(value: u32) -> i32 {
    ((value << 18) as i32) >> 18
}

/// Decode `TBZ/TBNZ Xn, #bit, target`.
fn decode_tb(insn: u32, insn_off: u32) -> Option<(bool, u32, u32, u32)> {
    if (insn & 0x7E00_0000) != 0x3600_0000 {
        return None;
    }
    let nz = (insn >> 24) & 1 != 0;
    let bit = (((insn >> 31) & 1) << 5) | ((insn >> 19) & 0x1F);
    let delta = sign_extend14((insn >> 5) & 0x3FFF) * 4;
    Some((nz, bit, insn & 0x1F, (insn_off as i32).wrapping_add(delta) as u32))
}

/// Encode an unconditional `B target`.
fn encode_b(insn_off: u32, target_off: u32) -> Option<u32> {
    let delta_bytes = target_off.wrapping_sub(insn_off) as i32;
    if (delta_bytes & 3) != 0 {
        return None;
    }
    let delta_instr = delta_bytes >> 2;
    if delta_instr < -(1 << 25) || delta_instr >= (1 << 25) {
        return None;
    }
    Some(0x1400_0000 | ((delta_instr as u32) & 0x03FF_FFFF))
}

/// Locate an anchor and return the file offset of its ADRP+ADD pair.
fn anchor_adrp(active: &[u8], anchor: &[u8]) -> Result<u32, PatchOutcome> {
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
    Ok(adrp_off)
}

/// Decode a `BL` target.
fn bl_target(pc: u32, insn: u32) -> Option<u32> {
    if (insn & ARM64_BL_MASK) != ARM64_BL_PAT {
        return None;
    }
    let imm = ((insn & 0x03FF_FFFF) << 6) as i32 >> 6;
    Some((pc as i32).wrapping_add(imm.wrapping_mul(4)) as u32)
}

/// Is `word` at `off` already the `B target` we would write?
fn is_b_to(word: u32, off: u32, target: u32) -> bool {
    match decode_branch(word, off) {
        Some(d) => d.kind == Arm64InsnKind::B && d.target_off == target,
        None => false,
    }
}

/// Anchors and offsets, resolved once.
struct Plan {
    gate_off: u32,
    gate_done: bool,
    /// `TBZ X19, #63, <launch>` right after the refusal reset.
    status_off: u32,
    status_done: bool,
    /// `TBNZ X19, #63, <spin>` that terminates the `LABEL_341` tail.
    tail_off: u32,
    tail_done: bool,
    /// `CBNZ W26, REFUSE` right before the tail test.
    tail_gate_off: u32,
    tail_gate_done: bool,
    /// `CBZ W26, LABEL_341` in the `OplusInitRecordData` failure block.
    init_gate_off: u32,
    init_gate_done: bool,
    /// `BL <ResetSystem>` of the `fastboot_unlock_verify` refusal.
    verify_call_off: u32,
    verify_done: bool,
    /// `LABEL_342` — the "Launching fastboot" print.
    launch_off: u32,
}

fn plan(active: &[u8]) -> Result<Plan, PatchOutcome> {
    // --- the dead-spin print anchors LABEL_342 and the spin itself ----
    let nosupport_adrp = anchor_adrp(active, ANCHOR_NO_SUPPORT)?;
    let launch_off = nosupport_adrp.saturating_add(0x70);
    let spin_off = nosupport_adrp.saturating_add(0x10);
    if (launch_off as usize) + 4 > active.len() {
        return Err(PatchOutcome::Miss);
    }
    // The spin block must really end in an unconditional self-branch.
    match decode_branch(read_instr_u32(active, spin_off), spin_off) {
        Some(d) if d.kind == Arm64InsnKind::B && d.target_off == spin_off => {}
        _ => return Err(PatchOutcome::Miss),
    }

    // --- the olock fastboot refusal -----------------------------------
    let locked_adrp = anchor_adrp(active, ANCHOR_LOCKED_FASTBOOT)?;
    let reset_mov = locked_adrp.saturating_add(OFF_RESET_REASON);
    let reset_call = locked_adrp.saturating_add(OFF_RESET_CALL);
    let status_off = locked_adrp.saturating_add(OFF_STATUS_TEST);
    if (status_off as usize) + 4 > active.len() || locked_adrp < 4 {
        return Err(PatchOutcome::Miss);
    }
    if read_instr_u32(active, reset_mov) != ARM64_MOV_W0_RESET_REASON {
        return Err(PatchOutcome::Miss);
    }
    let reset_target = match bl_target(reset_call, read_instr_u32(active, reset_call)) {
        Some(t) => t,
        None => return Err(PatchOutcome::Miss),
    };

    // `TBZ X19, #63, LABEL_342` — and it must point at the launch print.
    let status_word = read_instr_u32(active, status_off);
    let status_done = is_b_to(status_word, status_off, launch_off);
    let status_reg = if status_done {
        // Already rewritten; the register is no longer encoded, so read
        // it from the tail test below instead.
        0xFF
    } else {
        match decode_tb(status_word, status_off) {
            Some((false, 63, reg, target)) if target == launch_off => reg,
            _ => return Err(PatchOutcome::Miss),
        }
    };

    // Gate: `CBZ/CBNZ W26, ...` in front of the anchor, or our `B`.
    let gate_off = locked_adrp - 4;
    let gate_word = read_instr_u32(active, gate_off);
    let gate_done = is_b_to(gate_word, gate_off, launch_off);
    if !gate_done {
        match decode_branch(gate_word, gate_off) {
            Some(d)
                if matches!(
                    d.kind,
                    Arm64InsnKind::CbzW
                        | Arm64InsnKind::CbnzW
                        | Arm64InsnKind::CbzX
                        | Arm64InsnKind::CbnzX
                ) => {}
            _ => return Err(PatchOutcome::Miss),
        }
    }

    // `LABEL_341` tail: the `TBNZ Xn, #63, <spin>` that both the
    // non-fastboot path and the fall-through after the reset reach.
    let tail_off = status_off.saturating_add(0x70);
    if (tail_off as usize) + 4 > active.len() {
        return Err(PatchOutcome::Miss);
    }
    let tail_word = read_instr_u32(active, tail_off);
    let tail_done = is_b_to(tail_word, tail_off, launch_off);
    if !tail_done {
        match decode_tb(tail_word, tail_off) {
            // The tail jumps to the *start* of the spin block (the
            // "No fastboot support" print), which then falls into the
            // `B .` at `spin_off`.
            Some((true, 63, reg, target))
                if target == nosupport_adrp && (status_reg == 0xFF || reg == status_reg) => {}
            _ => return Err(PatchOutcome::Miss),
        }
    } else if status_reg == 0xFF {
        // Both already rewritten; nothing left to cross-check.
    }

    // --- the fastboot_unlock_verify refusal ---------------------------
    let verify_adrp = anchor_adrp(active, ANCHOR_UNLOCK_VERIFY)?;
    let verify_call_off = verify_adrp.saturating_add(OFF_RESET_CALL);
    if (verify_call_off as usize) + 4 > active.len() {
        return Err(PatchOutcome::Miss);
    }
    if read_instr_u32(active, verify_adrp + OFF_RESET_REASON) != ARM64_MOV_W0_RESET_REASON {
        return Err(PatchOutcome::Miss);
    }
    let verify_word = read_instr_u32(active, verify_call_off);
    let verify_done = verify_word == ARM64_NOP;
    if !verify_done {
        // Both exits must route through the same reset helper; anything
        // else means this is a build whose failure path we do not know.
        match bl_target(verify_call_off, verify_word) {
            Some(t) if t == reset_target => {}
            _ => return Err(PatchOutcome::Miss),
        }
    }

    // `CBNZ W26, REFUSE` immediately before the tail test: the
    // non-fastboot path taken after `LoadImageAndAuth` fails.
    let tail_gate_off = tail_off - 4;
    let tail_gate_word = read_instr_u32(active, tail_gate_off);
    let tail_gate_done = is_b_to(tail_gate_word, tail_gate_off, launch_off);
    if !tail_gate_done {
        match decode_branch(tail_gate_word, tail_gate_off) {
            Some(d)
                if d.kind == Arm64InsnKind::CbnzW && d.target_off == locked_adrp => {}
            _ => return Err(PatchOutcome::Miss),
        }
    }

    // `CBZ W26, LABEL_341` in the `OplusInitRecordData` failure block,
    // followed by `B REFUSE`.
    let init_adrp = anchor_adrp(active, ANCHOR_INIT_RECORD)?;
    let init_gate_off = init_adrp.saturating_add(OFF_INIT_RECORD_GATE);
    if (init_gate_off as usize) + 8 > active.len() {
        return Err(PatchOutcome::Miss);
    }
    let init_gate_word = read_instr_u32(active, init_gate_off);
    let init_gate_done = is_b_to(init_gate_word, init_gate_off, launch_off);
    if !init_gate_done {
        let ok = match decode_branch(init_gate_word, init_gate_off) {
            Some(d) if d.kind == Arm64InsnKind::CbzW && d.target_off == tail_off => true,
            _ => false,
        };
        // The fall-through must be the `B REFUSE` that this site guards.
        let fall = read_instr_u32(active, init_gate_off + 4);
        let fall_ok = match decode_branch(fall, init_gate_off + 4) {
            Some(d) if d.kind == Arm64InsnKind::B && d.target_off == locked_adrp => true,
            _ => false,
        };
        if !ok || !fall_ok {
            return Err(PatchOutcome::Miss);
        }
    }

    // --- the fastboot_unlock_verify refusal ---------------------------
    let verify_adrp = anchor_adrp(active, ANCHOR_UNLOCK_VERIFY)?;
    let verify_call_off = verify_adrp.saturating_add(OFF_RESET_CALL);
    if (verify_call_off as usize) + 4 > active.len() {
        return Err(PatchOutcome::Miss);
    }
    if read_instr_u32(active, verify_adrp + OFF_RESET_REASON) != ARM64_MOV_W0_RESET_REASON {
        return Err(PatchOutcome::Miss);
    }
    let verify_word = read_instr_u32(active, verify_call_off);
    let verify_done = verify_word == ARM64_NOP;
    if !verify_done {
        // Both exits must route through the same reset helper; anything
        // else means this is a build whose failure path we do not know.
        match bl_target(verify_call_off, verify_word) {
            Some(t) if t == reset_target => {}
            _ => return Err(PatchOutcome::Miss),
        }
    }

    Ok(Plan {
        gate_off,
        gate_done,
        status_off,
        status_done,
        tail_off,
        tail_done,
        tail_gate_off,
        tail_gate_done,
        init_gate_off,
        init_gate_done,
        verify_call_off,
        verify_done,
        launch_off,
    })
}

/// patch12 apply function (v4).
pub fn apply(buf: &mut [u8], size: u32) -> PatchOutcome {
    let active = &mut buf[..size as usize];
    let p = match plan(active) {
        Ok(p) => p,
        Err(e) => return e,
    };

    for (off, done) in [
        (p.gate_off, p.gate_done),
        (p.status_off, p.status_done),
        (p.tail_off, p.tail_done),
        (p.tail_gate_off, p.tail_gate_done),
        (p.init_gate_off, p.init_gate_done),
    ] {
        if !done {
            match encode_b(off, p.launch_off) {
                Some(insn) => write_instr_u32(active, off, insn),
                None => return PatchOutcome::Miss,
            }
        }
    }
    if !p.verify_done {
        write_instr_u32(active, p.verify_call_off, ARM64_NOP);
    }

    PatchOutcome::Ok
}
