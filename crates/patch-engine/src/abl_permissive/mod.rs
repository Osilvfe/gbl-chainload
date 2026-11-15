//! ABL-permissive patches — always compiled (firmware + host).
//!
//! Port of `GblChainloadPkg/Library/DynamicPatchLib/abl_permissive/`.
//! Three cross-OEM patch entries:
//!
//! - [`libavb_force_success`] — patch10. Forces libavb's
//!   `avb_slot_verify` to always return OK with populated SlotData,
//!   bypassing every `!allow_verification_error` gate.
//! - [`fastboot_lock_gates`] — patch6. Rewrites the four lock-state
//!   refusal gates in ABL's fastboot command dispatcher.
//! - [`locked_handoff`] — patch11 (optional). Bootconfig
//!   locked-handoff: ABL keeps its real unlocked state internally, but
//!   outbound `androidboot.vbmeta.device_state` always says `locked`
//!   and the verified-boot color table always answers `green`. It also
//!   includes 11c, which publishes `androidboot.vbmeta.*` for a
//!   verification-disabled top-level vbmeta.
//! Oplus-specific patch12/13 live in [`crate::oem::oplus`], not in this
//! always-on group.
//! [`vbmeta_disabled_cmdline`] is the implementation of patch11c and is
//! not a separate table entry.

pub mod libavb_force_success;
pub mod fastboot_lock_gates;
pub mod locked_handoff;
pub mod vbmeta_disabled_cmdline;

use crate::{PatchDesc, PatchScope};

/// All ABL-permissive patches. Always linked into both firmware and
/// host staticlibs.
pub const ABL_PERMISSIVE_PATCHES: &[PatchDesc] = &[
    PatchDesc {
        name: "patch10-libavb-force-avb-success",
        scope: PatchScope::AblPermissive,
        mandatory: true,
        apply: libavb_force_success::apply,
    },
    PatchDesc {
        name: "patch6-lock-state-fastboot-gate",
        scope: PatchScope::AblPermissive,
        mandatory: true,
        apply: fastboot_lock_gates::apply,
    },
    PatchDesc {
        name: "patch11-locked-handoff-bootconfig",
        scope: PatchScope::AblPermissive,
        mandatory: false,
        apply: locked_handoff::apply,
    },
];
