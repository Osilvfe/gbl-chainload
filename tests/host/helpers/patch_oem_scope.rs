use std::{env, fs, path::Path};

use patch_engine::{
    internal::{
        arm64_decode::find_adrp_add_targeting,
        encode::read_instr_u32,
        scan::{scan_for, ScanResult},
    },
    oem::oplus::{self, boot_failure_fallback, slot_unbootable_clear},
    Engine, Oem, PatchApply, PatchOutcome, Worst,
};

fn changed_words(original: &[u8], patched: &[u8]) -> Vec<usize> {
    assert_eq!(original.len(), patched.len());
    original
        .chunks(4)
        .zip(patched.chunks(4))
        .enumerate()
        .filter_map(|(index, (before, after))| (before != after).then_some(index * 4))
        .collect()
}

fn anchor_ref(image: &[u8], anchor: &[u8]) -> u32 {
    let (result, string_offset) = scan_for(image, anchor, None);
    assert_eq!(result, ScanResult::Found);
    let (result, reference) = find_adrp_add_targeting(image, string_offset, true);
    assert_eq!(result, ScanResult::Found);
    reference
}

fn apply_checked(original: &[u8], apply: PatchApply, expected: PatchOutcome) -> Vec<u8> {
    let mut patched = original.to_vec();
    let size = original.len() as u32;
    assert_eq!(apply(&mut patched, size), expected);
    let snapshot = patched.clone();
    assert_eq!(apply(&mut patched, size), expected);
    assert_eq!(patched, snapshot);
    if expected == PatchOutcome::Miss {
        assert_eq!(patched, original);
    }
    patched
}

fn main() {
    let mut oplus_count = 0;
    let mut other_count = 0;
    for argument in env::args().skip(1) {
        let path = Path::new(&argument);
        let name = path.file_name().unwrap().to_str().unwrap();
        let original = fs::read(path).unwrap();
        let is_oplus = name.starts_with("op");
        assert_eq!(oplus::matches_abl(&original), is_oplus, "{name}");
        let expected = if is_oplus {
            PatchOutcome::Ok
        } else {
            PatchOutcome::Miss
        };
        let patch12 = apply_checked(&original, boot_failure_fallback::apply, expected);
        let patch13 = apply_checked(&original, slot_unbootable_clear::apply, expected);

        let mut common = original.clone();
        let common_result = Engine::ensure_init_scoped(Oem::None, true).apply(&mut common);
        assert_eq!(common_result.applied_count, 3);
        assert_eq!(common_result.missed_count, 0);
        assert_eq!(common_result.worst, Worst::Ok);
        assert_eq!(common, fs::read(path.with_extension("none.efi")).unwrap());

        let offsets12 = changed_words(&original, &patch12);
        let offsets13 = changed_words(&original, &patch13);
        for offset in offsets12.iter().chain(&offsets13) {
            assert_eq!(
                &common[*offset..*offset + 4],
                &original[*offset..*offset + 4]
            );
        }
        let mut expected_firmware = common.clone();
        for patch in oplus::OPLUS_BOOT_PATCHES {
            assert_eq!(
                (patch.apply)(&mut expected_firmware, original.len() as u32),
                expected
            );
        }
        let mut firmware = original.clone();
        let firmware_result = Engine::ensure_init().apply(&mut firmware);
        assert_eq!(firmware, expected_firmware);
        assert_eq!(firmware_result.applied_count, if is_oplus { 5 } else { 3 });
        assert_eq!(firmware_result.missed_count, 0);
        assert_eq!(firmware_result.worst, Worst::Ok);

        let mut host = original.clone();
        let host_result =
            Engine::ensure_init_scoped(if is_oplus { Oem::Oplus } else { Oem::None }, true)
                .apply(&mut host);
        assert_eq!(host_result.applied_count, if is_oplus { 6 } else { 3 });
        assert_eq!(host_result.missed_count, 0);
        assert_eq!(host_result.worst, Worst::Ok);
        assert_eq!(host, fs::read(path.with_extension("selected.efi")).unwrap());

        if is_oplus {
            assert_eq!(offsets12.len(), 6);
            assert_eq!(offsets13.len(), 3);
            let locked = anchor_ref(
                &original,
                b"if olock secure lock state, Not allow Fastboot.",
            );
            let verify = anchor_ref(&original, b"fastboot_unlock_verify error and reboot");
            let no_support = anchor_ref(
                &original,
                b"No fastboot support for flashless chipsets, Infinte loop",
            );
            let init = anchor_ref(&original, b"OplusInitRecordData failed: %r\n");
            let launch = no_support + 0x70;
            let branch_sites = [
                locked - 4,
                locked + 0x18,
                locked + 0x88,
                locked + 0x84,
                init + 0x1c,
            ];
            let mut expected_sites: Vec<usize> =
                branch_sites.iter().map(|offset| *offset as usize).collect();
            expected_sites.push((verify + 0x14) as usize);
            expected_sites.sort_unstable();
            assert_eq!(offsets12, expected_sites);
            for offset in branch_sites {
                let word = read_instr_u32(&patch12, offset);
                assert_eq!(word >> 26, 5);
                let immediate = ((word & 0x03ff_ffff) as i32) << 6 >> 6;
                assert_eq!(offset.wrapping_add((immediate * 4) as u32), launch);
            }
            assert_eq!(read_instr_u32(&patch12, verify + 0x14), 0xd503201f);
            for (before, after) in [
                (0xd3607d08u32, 0xaa0903e8u32),
                (0xd3575f78, 0xaa1f03f8),
                (0x53175d08, 0x2a1f03e8),
            ] {
                assert_eq!(
                    offsets13
                        .iter()
                        .filter(|offset| {
                            read_instr_u32(&original, **offset as u32) == before
                                && read_instr_u32(&patch13, **offset as u32) == after
                        })
                        .count(),
                    1
                );
            }
            for preserved in [0x32090129u32, 0x79400d28u32] {
                let offsets: Vec<usize> = original
                    .windows(4)
                    .enumerate()
                    .filter_map(|(offset, word)| {
                        (word == preserved.to_le_bytes()).then_some(offset)
                    })
                    .collect();
                assert!(!offsets.is_empty());
                for offset in offsets {
                    assert_eq!(&patch13[offset..offset + 4], &original[offset..offset + 4]);
                }
            }
            let mut expected_host = firmware;
            assert_eq!(
                oplus::bypass_warning::apply(&mut expected_host, original.len() as u32),
                PatchOutcome::Ok
            );
            assert_eq!(host, expected_host);
            oplus_count += 1;
            println!(
                "  ok: {name} — Oplus host + firmware enabled; none skips; 6+3 edits verified"
            );
        } else {
            assert_eq!(host, common);
            other_count += 1;
            println!("  ok: {name} — host + firmware skip Oplus patches entirely");
        }
    }
    assert!(oplus_count + other_count > 0);
    println!("PASS: OEM matrix: {oplus_count} Oplus, {other_count} non-Oplus");
}
