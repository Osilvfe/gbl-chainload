//! Cross-OEM patches enabled only for the profile-spoof mode.
//!
//! Patch11a/11b/11c form one idempotent locked-handoff patch. Host
//! packaging selects it with `--mode 2`; firmware selects it from the
//! loaded manifest's `WantProfileSpoof` bit.

pub mod locked_handoff;
pub mod vbmeta_disabled_cmdline;

use crate::{PatchDesc, PatchScope};

/// The complete patch11 entry, including its 11c vbmeta handoff edit.
pub const MODE2_PATCHES: &[PatchDesc] = &[PatchDesc {
    name: "patch11-locked-handoff-bootconfig",
    scope: PatchScope::Mode2,
    mandatory: false,
    apply: locked_handoff::apply,
}];
