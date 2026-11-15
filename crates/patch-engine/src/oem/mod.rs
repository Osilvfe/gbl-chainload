//! OEM-specific patches for host packaging and firmware fallback.
//!
//! Each OEM lives in its own submodule (`oplus`, etc.); the engine
//! aggregates them by [`crate::Oem`] selector in
//! [`crate::Engine::ensure_init_scoped`]. Firmware fallback selects the
//! Oplus boot patches only when its ABL vendor markers are present.

pub mod oplus;
