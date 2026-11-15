//! OnePlus / Oppo / Realme (oplus / canoe) family OEM patches.
//!
//! Host packaging enables patch7/12/13 with `gbl patch --oem oplus`.
//! Firmware fallback detects Oplus ABL vendor markers and enables only
//! patch12/13; patch7 remains host-only.

#[cfg(feature = "host")]
pub mod bypass_warning;
pub mod boot_failure_fallback;
pub mod slot_unbootable_clear;

use crate::{PatchDesc, PatchScope};

/// Host-only Oplus unlock-warning patch.
#[cfg(feature = "host")]
pub const OEM_OPLUS_PATCHES: &[PatchDesc] = &[
    PatchDesc {
        name: "patch7-orange-screen",
        scope: PatchScope::OemOplus,
        mandatory: false,
        apply: bypass_warning::apply,
    },
];

/// Oplus boot-failure patches shared by packaging and firmware fallback.
pub const OPLUS_BOOT_PATCHES: &[PatchDesc] = &[
    PatchDesc {
        name: "patch12-boot-failure-fastboot-fallback",
        scope: PatchScope::OemOplus,
        mandatory: false,
        apply: boot_failure_fallback::apply,
    },
    PatchDesc {
        name: "patch13-slot-unbootable-inert",
        scope: PatchScope::OemOplus,
        mandatory: false,
        apply: slot_unbootable_clear::apply,
    },
];

/// Recognize the Oplus boot flow independently of its instruction layout.
pub fn matches_abl(image: &[u8]) -> bool {
    [
        b"if olock secure lock state, Not allow Fastboot.".as_slice(),
        b"OplusInitRecordData failed: %r\n".as_slice(),
    ]
    .iter()
    .all(|marker| image.windows(marker.len()).any(|window| window == *marker))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn detection_requires_both_vendor_markers() {
        assert!(!matches_abl(b""));
        assert!(!matches_abl(
            b"if olock secure lock state, Not allow Fastboot."
        ));
        assert!(!matches_abl(b"OplusInitRecordData failed: %r\n"));
        assert!(matches_abl(
            b"if olock secure lock state, Not allow Fastboot.\0OplusInitRecordData failed: %r\n"
        ));
    }
}
