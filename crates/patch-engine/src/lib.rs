//! Dynamic patch engine — the Rust replacement for
//! `GblChainloadPkg/Library/DynamicPatchLib/`.
//!
//! Mirrors PR1's directory shape:
//!
//! - [`abl_permissive`] — always-on cross-OEM patch10 + patch6.
//! - [`mode2`] — patch11, enabled only for profile spoofing.
//! - [`oem`] — Oplus patch12/13, plus host-only patch7.
//! - [`retired`] — host-only documentation / reference. patch1 (efisp
//!   recursion) — never wired into an active table.
//!
//! Two surfaces:
//!
//! 1. Idiomatic Rust API at the crate root: [`Engine`], [`PatchDesc`],
//!    [`PatchScope`], [`PatchOutcome`], [`Oem`], [`PatchResult`],
//!    [`Worst`].
//! 2. The [`ffi`] module — `extern "C"` shims preserving the C wire ABI
//!    of the deleted `DynamicPatchLib.h` / `PatchScope.h` so the
//!    firmware (`BootFlow.c`) and host C tools (`abl-patcher`) link
//!    `libpatch_engine.a` and keep working unchanged.
//!
//! `no_std` when the `std` feature is off (firmware build with
//! `--no-default-features`). Hosted builds keep `std` so cargo's test
//! harness can unwind.

#![cfg_attr(not(feature = "std"), no_std)]

// In the firmware build libpatch_engine.a, libgblp1.a, and
// libmode2_profile_core.a are all linked into the same EDK2 image.
// Each `#[panic_handler]` lowers to a strong `rust_begin_unwind`
// symbol; the EDK2 link line passes `-Wl,--allow-multiple-definition`
// so the three identical `loop {}` panic handlers coexist.
#[cfg(not(feature = "std"))]
#[panic_handler]
fn panic(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}

pub mod internal;
pub mod abl_permissive;
pub mod mode2;
pub mod oem;
#[cfg(feature = "host")]
pub mod retired;
pub mod ffi;

// --- Wire enums --------------------------------------------------------
//
// Discriminants of every public enum here are part of the FFI wire ABI;
// they match the deleted C headers byte-for-byte and are asserted by
// the `wire_discriminants` test below + the parity test in
// `tests/parity.rs`.

/// OEM group selector — `NONE = universal only`. Host tool surface.
///
/// Discriminants match `enum GBL_OEM` from the deleted
/// `DynamicPatchLib/PatchScope.h`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Oem {
    None = 0,
    Oplus = 1,
}

/// Patch scope — which group a [`PatchDesc`] belongs to.
///
/// Existing discriminants match the deleted `PATCH_SCOPE`; the mode-2
/// scope is appended without renumbering the original values.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum PatchScope {
    Universal = 0,
    OemOplus = 1,
    AblPermissive = 2,
    Mode2 = 3,
}

/// Per-patch outcome.
///
/// Discriminants match `enum PATCH_OUTCOME` from the deleted
/// `Include/Library/PatchDesc.h`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum PatchOutcome {
    Ok = 0,
    Miss = 1,
    Ambiguous = 2,
}

/// Aggregate worst-case outcome — what the caller (BootFlow.c) keys off
/// to decide whether to abort the chainload.
///
/// Discriminants match `enum PATCH_WORST` from the deleted
/// `Include/Library/DynamicPatchLib.h`.
#[repr(C)]
#[derive(Debug, PartialEq, Eq, Clone, Copy)]
pub enum Worst {
    Ok = 0,
    OptionalMiss = 1,
    MandatoryMiss = 2,
}

/// Per-patch function pointer signature.
pub type PatchApply = fn(buf: &mut [u8], size: u32) -> PatchOutcome;

/// A single patch in the engine's table.
#[derive(Clone, Copy)]
pub struct PatchDesc {
    pub name: &'static str,
    pub scope: PatchScope,
    pub mandatory: bool,
    pub apply: PatchApply,
}

/// Aggregate result of [`Engine::apply`] — applied + missed counts plus
/// the worst-case outcome across the whole table.
#[derive(Debug, Clone, Copy)]
pub struct PatchResult {
    pub applied_count: u32,
    pub missed_count: u32,
    pub worst: Worst,
}

/// The patch engine — a table of [`PatchDesc`] + an `apply()` driver.
///
/// Owned-vs-borrowed: the host scoped path stores an owned `Vec<PatchDesc>`
/// (no static state, safe in parallel tests). The firmware path
/// borrows from the `&'static [PatchDesc]` constants. The
/// `EngineTable` enum hides this split.
#[derive(Clone)]
pub struct Engine {
    table: EngineTable,
    detect_oplus: bool,
    mode2: bool,
}

#[derive(Clone)]
enum EngineTable {
    /// Firmware path / non-allocating callers — borrow from
    /// `&'static [PatchDesc]`.
    Static(&'static [PatchDesc]),
    /// Host scoped path — owned aggregate built at `ensure_init_scoped`
    /// time. Avoids any static mutable state so the tests can run in
    /// parallel without trampling each other.
    #[cfg(feature = "std")]
    Owned(alloc_vec::Vec<PatchDesc>),
}

#[cfg(feature = "std")]
mod alloc_vec {
    pub use std::vec::Vec;
}

impl EngineTable {
    fn as_slice(&self) -> &[PatchDesc] {
        match self {
            EngineTable::Static(s) => s,
            #[cfg(feature = "std")]
            EngineTable::Owned(v) => v.as_slice(),
        }
    }
}

impl Engine {
    /// Firmware path: cross-OEM patches, plus patch12/13 only when the
    /// input ABL contains both Oplus vendor markers. No allocation is
    /// needed, and patch7 remains excluded from this path.
    pub fn ensure_init() -> Engine {
        Engine {
            table: EngineTable::Static(abl_permissive::ABL_PERMISSIVE_PATCHES),
            detect_oplus: true,
            mode2: false,
        }
    }

    /// Host-only: pick OEM group + abl_permissive inclusion at runtime.
    ///
    /// Mirrors `DynamicPatchLib_EnsureInitScoped()` from the deleted
    /// `PatchTable.c`. Builds a `Vec<PatchDesc>` owned by the returned
    /// engine — no static state, parallel-test-safe.
    #[cfg(feature = "host")]
    pub fn ensure_init_scoped(oem: Oem, include_abl_permissive: bool) -> Engine {
        let mut v: alloc_vec::Vec<PatchDesc> = alloc_vec::Vec::with_capacity(8);
        // Retired group is empty post-Task 10 — no append.
        if oem == Oem::Oplus {
            v.extend_from_slice(oem::oplus::OEM_OPLUS_PATCHES);
        }
        if include_abl_permissive {
            v.extend_from_slice(abl_permissive::ABL_PERMISSIVE_PATCHES);
        }
        if oem == Oem::Oplus {
            v.extend_from_slice(oem::oplus::OPLUS_BOOT_PATCHES);
        }
        Engine {
            table: EngineTable::Owned(v),
            detect_oplus: false,
            mode2: false,
        }
    }

    /// Enable all three parts of patch11 only for mode-2/profile spoofing.
    pub fn with_mode2(mut self, enabled: bool) -> Self {
        self.mode2 = enabled;
        self
    }

    /// Apply every patch in the table to `pe` in-place.
    ///
    /// Mirrors `DynamicPatch_Apply` from the deleted
    /// `Internal/PatchEngine.c`.
    pub fn apply(&self, pe: &mut [u8]) -> PatchResult {
        let size = pe.len() as u32;
        let mut applied: u32 = 0;
        let mut missed: u32 = 0;
        let mut worst = Worst::Ok;

        let oplus_patches = if self.detect_oplus && oem::oplus::matches_abl(pe) {
            oem::oplus::OPLUS_BOOT_PATCHES
        } else {
            &[]
        };
        let mode2_patches = if self.mode2 {
            mode2::MODE2_PATCHES
        } else {
            &[]
        };
        for p in self
            .table
            .as_slice()
            .iter()
            .chain(mode2_patches)
            .chain(oplus_patches)
        {
            let outcome = (p.apply)(pe, size);
            self.emit_log(p, outcome);
            match outcome {
                PatchOutcome::Ok => applied += 1,
                _ => {
                    missed += 1;
                    let next = if p.mandatory {
                        Worst::MandatoryMiss
                    } else {
                        Worst::OptionalMiss
                    };
                    if (next as u32) > (worst as u32) {
                        worst = next;
                    }
                }
            }
        }

        PatchResult {
            applied_count: applied,
            missed_count: missed,
            worst,
        }
    }

    /// Borrow the base table, before mode-2 selection and firmware ABL detection.
    #[doc(hidden)]
    pub fn table(&self) -> &[PatchDesc] {
        self.table.as_slice()
    }

    /// Construct an Engine from a static slice — used internally by the
    /// test scaffolding and (potentially) by external host callers that
    /// want to drive a custom table.
    #[doc(hidden)]
    pub fn from_static(table: &'static [PatchDesc]) -> Engine {
        Engine {
            table: EngineTable::Static(table),
            detect_oplus: false,
            mode2: false,
        }
    }

    #[cfg(feature = "std")]
    fn emit_log(&self, p: &PatchDesc, outcome: PatchOutcome) {
        let outcome_name = match outcome {
            PatchOutcome::Ok => "OK",
            PatchOutcome::Miss => "MISS",
            PatchOutcome::Ambiguous => "AMBIGUOUS",
        };
        let scope_name = match p.scope {
            PatchScope::Universal => "universal",
            PatchScope::OemOplus => "oem-oplus",
            PatchScope::AblPermissive => "abl-permissive",
            PatchScope::Mode2 => "mode-2",
        };
        let mandatory_str = if p.mandatory { "mandatory" } else { "optional" };
        eprintln!(
            "DynamicPatch: {} [{}, {}] -> {}",
            p.name, scope_name, mandatory_str, outcome_name
        );
    }

    #[cfg(not(feature = "std"))]
    fn emit_log(&self, _p: &PatchDesc, _outcome: PatchOutcome) {
        // Firmware path: the FFI shim emits the screen line and the
        // DebugLib record. We don't have stdio here.
    }
}


#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn wire_discriminants() {
        // Mirror of `enum PATCH_SCOPE` from the deleted PatchDesc.h.
        assert_eq!(PatchScope::Universal as u32, 0);
        assert_eq!(PatchScope::OemOplus as u32, 1);
        assert_eq!(PatchScope::AblPermissive as u32, 2);
        assert_eq!(PatchScope::Mode2 as u32, 3);
        // Mirror of `enum PATCH_OUTCOME`.
        assert_eq!(PatchOutcome::Ok as u32, 0);
        assert_eq!(PatchOutcome::Miss as u32, 1);
        assert_eq!(PatchOutcome::Ambiguous as u32, 2);
        // Mirror of `enum PATCH_WORST`.
        assert_eq!(Worst::Ok as u32, 0);
        assert_eq!(Worst::OptionalMiss as u32, 1);
        assert_eq!(Worst::MandatoryMiss as u32, 2);
        // Mirror of `enum GBL_OEM` from the deleted PatchScope.h.
        assert_eq!(Oem::None as u32, 0);
        assert_eq!(Oem::Oplus as u32, 1);
    }

    fn stub_ok(_b: &mut [u8], _s: u32) -> PatchOutcome {
        PatchOutcome::Ok
    }
    fn stub_miss(_b: &mut [u8], _s: u32) -> PatchOutcome {
        PatchOutcome::Miss
    }
    fn stub_ambig(_b: &mut [u8], _s: u32) -> PatchOutcome {
        PatchOutcome::Ambiguous
    }

    // Mirror of tests/scan/test_engine.c.
    static ALL_OK_TABLE: &[PatchDesc] = &[
        PatchDesc {
            name: "p1",
            scope: PatchScope::Universal,
            mandatory: true,
            apply: stub_ok,
        },
        PatchDesc {
            name: "p2",
            scope: PatchScope::Universal,
            mandatory: false,
            apply: stub_ok,
        },
    ];

    static OPTIONAL_MISS_TABLE: &[PatchDesc] = &[
        PatchDesc {
            name: "p1",
            scope: PatchScope::Universal,
            mandatory: true,
            apply: stub_ok,
        },
        PatchDesc {
            name: "p2",
            scope: PatchScope::OemOplus,
            mandatory: false,
            apply: stub_miss,
        },
    ];

    static MANDATORY_MISS_TABLE: &[PatchDesc] = &[
        PatchDesc {
            name: "p1",
            scope: PatchScope::Universal,
            mandatory: true,
            apply: stub_miss,
        },
        PatchDesc {
            name: "p2",
            scope: PatchScope::Universal,
            mandatory: false,
            apply: stub_ok,
        },
    ];

    static AMBIGUOUS_TABLE: &[PatchDesc] = &[PatchDesc {
        name: "p1",
        scope: PatchScope::Universal,
        mandatory: true,
        apply: stub_ambig,
    }];

    #[test]
    fn engine_all_ok() {
        let eng = Engine::from_static(ALL_OK_TABLE);
        let mut buf = [0u8; 16];
        let r = eng.apply(&mut buf);
        assert_eq!(r.applied_count, 2);
        assert_eq!(r.missed_count, 0);
        assert_eq!(r.worst, Worst::Ok);
    }

    #[test]
    fn engine_optional_miss() {
        let eng = Engine::from_static(OPTIONAL_MISS_TABLE);
        let mut buf = [0u8; 16];
        let r = eng.apply(&mut buf);
        assert_eq!(r.applied_count, 1);
        assert_eq!(r.missed_count, 1);
        assert_eq!(r.worst, Worst::OptionalMiss);
    }

    #[test]
    fn engine_mandatory_miss() {
        let eng = Engine::from_static(MANDATORY_MISS_TABLE);
        let mut buf = [0u8; 16];
        let r = eng.apply(&mut buf);
        assert_eq!(r.applied_count, 1);
        assert_eq!(r.missed_count, 1);
        assert_eq!(r.worst, Worst::MandatoryMiss);
    }

    #[test]
    fn engine_ambiguous_counts_as_miss() {
        let eng = Engine::from_static(AMBIGUOUS_TABLE);
        let mut buf = [0u8; 16];
        let r = eng.apply(&mut buf);
        assert_eq!(r.applied_count, 0);
        assert_eq!(r.missed_count, 1);
        assert_eq!(r.worst, Worst::MandatoryMiss);
    }

    #[test]
    fn engine_empty_table() {
        let eng = Engine::from_static(&[]);
        let mut buf = [0u8; 16];
        let r = eng.apply(&mut buf);
        assert_eq!(r.applied_count, 0);
        assert_eq!(r.missed_count, 0);
        assert_eq!(r.worst, Worst::Ok);
    }

    #[test]
    fn mode2_must_be_explicitly_enabled_and_can_be_disabled() {
        let mut buffer = [0u8; 4096];
        let engine = Engine::from_static(&[]);
        assert_eq!(engine.apply(&mut buffer).missed_count, 0);
        let enabled = engine.with_mode2(true);
        let result = enabled.apply(&mut buffer);
        assert_eq!(result.applied_count, 0);
        assert_eq!(result.missed_count, 1);
        assert_eq!(result.worst, Worst::OptionalMiss);
        assert_eq!(enabled.with_mode2(false).apply(&mut buffer).missed_count, 0);
    }

    #[cfg(feature = "host")]
    #[test]
    fn ensure_init_scoped_skips_oem() {
        let eng = Engine::ensure_init_scoped(Oem::None, true);
        assert_eq!(eng.table().len(), 2);
        for p in eng.table() {
            assert_eq!(p.scope, PatchScope::AblPermissive);
        }
    }

    #[cfg(feature = "host")]
    #[test]
    fn ensure_init_scoped_includes_oem() {
        let eng = Engine::ensure_init_scoped(Oem::Oplus, true);
        assert_eq!(eng.table().len(), 5);
        assert_eq!(eng.table()[0].scope, PatchScope::OemOplus);
    }

    #[cfg(feature = "host")]
    #[test]
    fn ensure_init_scoped_oem_only() {
        let eng = Engine::ensure_init_scoped(Oem::Oplus, false);
        assert_eq!(eng.table().len(), 3);
        for patch in eng.table() {
            assert_eq!(patch.scope, PatchScope::OemOplus);
        }
    }

    #[test]
    fn firmware_engine_detects_oplus_separately_from_common_table() {
        let eng = Engine::ensure_init();
        assert!(eng.detect_oplus);
        assert_eq!(eng.table().len(), 2);
        for p in eng.table() {
            assert_eq!(p.scope, PatchScope::AblPermissive);
        }
    }
}
