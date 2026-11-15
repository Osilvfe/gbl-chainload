//! `gbl slot` — read/write the A/B slot attributes that Qualcomm ABL keeps in
//! the GPT partition entries of `boot_a` / `boot_b`.
//!
//! The attribute word is a `UINT64` at `entry + 0x30`
//! (`QcomModulePkg/Include/Library/PartitionTableUpdate.h`):
//!
//! ```text
//!   bits 48..49  priority
//!   bit  50      active
//!   bits 51..53  max retry count
//!   bit  54      successful
//!   bit  55      unbootable
//! ```
//!
//! ABL's `UpdatePartitionAttributes()` rewrites **both** the primary and the
//! backup GPT and recomputes the entry-array CRC and the header CRC of each.
//! This command does the same, so a write here is equivalent to what ABL does
//! on `fastboot set_active` / `HandleActiveSlotUnbootable`.
//!
//! Safety rules:
//!   * both GPT headers and both entry arrays must validate before anything is
//!     written (`show` reports the primary only, `set`/`restore` require both);
//!   * both `boot_a` and `boot_b` must be present;
//!   * writing `unbootable = 1` on the slot whose peer is already unbootable is
//!     refused unless `--force` — that is the state stock ABL cannot leave.

use std::fs::{File, OpenOptions};
use std::io::{self, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};

use clap::{Parser, Subcommand};

use super::GblError;

const PART_ATTR_OFF: usize = 0x30;
const PART_NAME_OFF: usize = 0x38;
const PART_NAME_LEN: usize = 72; // bytes of UTF-16LE

const ATTR_PRIORITY_BIT: u32 = 48;
const ATTR_ACTIVE_BIT: u32 = 50;
const ATTR_RETRY_BIT: u32 = 51;
const ATTR_SUCCESS_BIT: u32 = 54;
const ATTR_UNBOOTABLE_BIT: u32 = 55;

const MAX_ENTRIES: u32 = 1024;
const MIN_ENTRY_SIZE: u32 = 128;
const MAX_ENTRY_SIZE: u32 = 1024;

#[derive(Parser, Debug)]
#[command(about = "Read/write A/B slot attributes in the GPT (boot_a/boot_b)")]
pub struct Args {
    #[command(subcommand)]
    cmd: Cmd,
}

#[derive(Subcommand, Debug)]
enum Cmd {
    /// Print both slots' attributes (read-only)
    Show(ShowArgs),
    /// Rewrite one slot's attributes
    Set(SetArgs),
    /// Save the GPT (headers + entry arrays) to a file
    Backup(BackupArgs),
    /// Restore a GPT saved by `gbl slot backup`
    Restore(RestoreArgs),
}

#[derive(clap::Args, Debug, Clone)]
struct DiskOpt {
    /// Whole-disk node or image file. Default: derive from
    /// /dev/block/by-name/boot_a, else scan /dev/block, else fastboot.
    #[arg(long, value_name = "PATH")]
    disk: Option<PathBuf>,
    /// Force the logical block size instead of probing 512 then 4096.
    #[arg(long, value_name = "BYTES")]
    lba: Option<u32>,
    /// Read over fastboot `getvar` (read-only, no root). Only
    /// successful / unbootable / retry are published by ABL.
    #[arg(long, conflicts_with_all = ["disk", "adb"])]
    fastboot: bool,
    /// Run the module binary on the device over `adb shell su -c`
    /// (gives every field, and can write).
    #[arg(long, conflicts_with_all = ["disk", "fastboot"])]
    adb: bool,
    /// fastboot serial / adb serial
    #[arg(long, value_name = "SN")]
    serial: Option<String>,
    /// On-device `gbl` path used by --adb.
    #[arg(long, value_name = "PATH", default_value = DEVICE_BIN)]
    device_bin: String,
}

#[derive(clap::Args, Debug)]
struct ShowArgs {
    #[command(flatten)]
    disk: DiskOpt,
}

#[derive(clap::Args, Debug)]
struct SetArgs {
    /// Slot suffix: a or b
    #[arg(long, value_name = "a|b")]
    slot: String,
    #[arg(long, value_name = "0|1")]
    unbootable: Option<u8>,
    #[arg(long, value_name = "0|1")]
    successful: Option<u8>,
    #[arg(long, value_name = "0|1")]
    active: Option<u8>,
    #[arg(long, value_name = "0-3")]
    priority: Option<u8>,
    #[arg(long, value_name = "0-7")]
    retry: Option<u8>,
    /// Compute the new attributes but do not write
    #[arg(long)]
    dry_run: bool,
    /// Allow marking both slots unbootable
    #[arg(long)]
    force: bool,
    #[command(flatten)]
    disk: DiskOpt,
}

#[derive(clap::Args, Debug)]
struct BackupArgs {
    #[arg(long, value_name = "FILE")]
    out: PathBuf,
    #[command(flatten)]
    disk: DiskOpt,
}

#[derive(clap::Args, Debug)]
struct RestoreArgs {
    #[arg(long, value_name = "FILE")]
    r#in: PathBuf,
    /// Write even if the image geometry does not match the target
    #[arg(long)]
    force: bool,
    #[command(flatten)]
    disk: DiskOpt,
}

// ---------------------------------------------------------------------------
// little-endian helpers
// ---------------------------------------------------------------------------

fn le32(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

fn le64(b: &[u8], o: usize) -> u64 {
    u64::from_le_bytes([
        b[o],
        b[o + 1],
        b[o + 2],
        b[o + 3],
        b[o + 4],
        b[o + 5],
        b[o + 6],
        b[o + 7],
    ])
}

fn put32(b: &mut [u8], o: usize, v: u32) {
    b[o..o + 4].copy_from_slice(&v.to_le_bytes());
}

fn put64(b: &mut [u8], o: usize, v: u64) {
    b[o..o + 8].copy_from_slice(&v.to_le_bytes());
}

// ---------------------------------------------------------------------------
// GPT model
// ---------------------------------------------------------------------------

#[derive(Debug)]
struct Gpt {
    lba: u32,
    total_lba: u64,
    /// Primary header sector (`lba` bytes, CRC valid as loaded).
    hdr: Vec<u8>,
    hdr_sz: u32,
    alt_lba: u64,
    /// Primary entry array.
    entries: Vec<u8>,
    entry_lba: u64,
    /// Backup header sector and entry array.
    alt_hdr: Vec<u8>,
    alt_entry_lba: u64,
    alt_entries: Vec<u8>,
    nent: u32,
    esz: u32,
}

struct HdrInfo {
    hdr_sz: u32,
    alt_lba: u64,
    entry_lba: u64,
    nent: u32,
    esz: u32,
    ecrc: u32,
}

fn parse_hdr(sec: &[u8], lba: u32) -> Result<HdrInfo, &'static str> {
    if &sec[0..8] != b"EFI PART" {
        return Err("no EFI PART signature");
    }
    let hdr_sz = le32(sec, 12);
    if hdr_sz < 92 || hdr_sz as usize > lba as usize {
        return Err("bad header size");
    }
    let stored_crc = le32(sec, 16);
    let mut tmp = sec[..hdr_sz as usize].to_vec();
    put32(&mut tmp, 16, 0);
    if crc32fast::hash(&tmp) != stored_crc {
        return Err("header CRC mismatch");
    }
    let alt_lba = le64(sec, 32);
    let entry_lba = le64(sec, 72);
    let nent = le32(sec, 80);
    let esz = le32(sec, 84);
    let ecrc = le32(sec, 88);
    if nent == 0 || nent > MAX_ENTRIES {
        return Err("bad partition entry count");
    }
    if !(MIN_ENTRY_SIZE..=MAX_ENTRY_SIZE).contains(&esz) || esz % 8 != 0 {
        return Err("bad partition entry size");
    }
    Ok(HdrInfo {
        hdr_sz,
        alt_lba,
        entry_lba,
        nent,
        esz,
        ecrc,
    })
}

impl Gpt {
    fn read_lba<R: Read + Seek>(r: &mut R, lba: u64, bs: u32) -> io::Result<Vec<u8>> {
        r.seek(SeekFrom::Start(lba * bs as u64))?;
        let mut buf = vec![0u8; bs as usize];
        r.read_exact(&mut buf)?;
        Ok(buf)
    }

    fn read_entries<R: Read + Seek>(
        r: &mut R,
        at: u64,
        bs: u32,
        nent: u32,
        esz: u32,
    ) -> io::Result<Vec<u8>> {
        r.seek(SeekFrom::Start(at * bs as u64))?;
        let mut buf = vec![0u8; nent as usize * esz as usize];
        r.read_exact(&mut buf)?;
        Ok(buf)
    }

    /// Parse and fully validate a GPT. `forced_lba` skips the 512/4096 probe.
    fn parse<R: Read + Seek>(r: &mut R, forced_lba: Option<u32>) -> Result<Gpt, String> {
        let candidates: &[u32] = match forced_lba {
            Some(_) => &[],
            None => &[512, 4096],
        };
        let mut last_err = String::from("no GPT found");

        let mut order: Vec<u32> = candidates.to_vec();
        if let Some(v) = forced_lba {
            order.push(v);
        }
        for bs in order {
            if !(512..=4096).contains(&bs) {
                last_err = format!("unsupported block size {bs}");
                continue;
            }
            let hdr = match Self::read_lba(r, 1, bs) {
                Ok(b) => b,
                Err(e) => {
                    last_err = e.to_string();
                    continue;
                }
            };
            match parse_hdr(&hdr, bs) {
                Ok(HdrInfo {
                    hdr_sz,
                    alt_lba,
                    entry_lba,
                    nent,
                    esz,
                    ecrc,
                }) => {
                    // A header with a valid CRC at this block size pins the
                    // geometry: any later failure is real corruption, not a
                    // wrong block-size guess, so do not fall through to 4096.
                    let addr = |e: std::io::Error| format!("block size {bs}: {e}");
                    let entries = Self::read_entries(r, entry_lba, bs, nent, esz).map_err(addr)?;
                    if crc32fast::hash(&entries) != ecrc {
                        return Err(format!(
                            "block size {bs}: partition entry array CRC mismatch"
                        ));
                    }
                    let total_lba = r.seek(SeekFrom::End(0)).map_err(addr)? / bs as u64;
                    if alt_lba == 0 || alt_lba >= total_lba {
                        return Err(format!(
                            "block size {bs}: backup GPT LBA {alt_lba} outside disk"
                        ));
                    }
                    let alt_hdr = Self::read_lba(r, alt_lba, bs).map_err(addr)?;
                    let HdrInfo {
                        entry_lba: alt_entry_lba,
                        nent: anent,
                        esz: aesz,
                        ecrc: aecrc,
                        ..
                    } = parse_hdr(&alt_hdr, bs).map_err(|e| format!("backup header: {e}"))?;
                    if anent != nent || aesz != esz {
                        return Err("backup GPT geometry differs from primary".into());
                    }
                    let alt_entries =
                        Self::read_entries(r, alt_entry_lba, bs, anent, aesz).map_err(addr)?;
                    if crc32fast::hash(&alt_entries) != aecrc {
                        return Err("backup entry array CRC mismatch".into());
                    }
                    return Ok(Gpt {
                        lba: bs,
                        total_lba,
                        hdr,
                        hdr_sz,
                        alt_lba,
                        entries,
                        entry_lba,
                        alt_hdr,
                        alt_entry_lba,
                        alt_entries,
                        nent,
                        esz,
                    });
                }
                Err(e) => last_err = format!("{e} (block size {bs})"),
            }
        }
        Err(last_err)
    }

    fn slot_index(&self, suffix: &str) -> Option<usize> {
        let want = format!("boot_{suffix}");
        for i in 0..self.nent as usize {
            let e = &self.entries[i * self.esz as usize..(i + 1) * self.esz as usize];
            if e[0..16].iter().all(|&b| b == 0) {
                continue; // unused entry
            }
            if entry_name(e) == want {
                return Some(i);
            }
        }
        None
    }

    fn attr(&self, idx: usize) -> u64 {
        le64(&self.entries[idx * self.esz as usize..], PART_ATTR_OFF)
    }

    fn set_attr(&mut self, idx: usize, v: u64) {
        let off = idx * self.esz as usize + PART_ATTR_OFF;
        put64(&mut self.entries, off, v);
    }

    fn reseal(&mut self) {
        // Every attribute write goes through ABL's writer, which stores the
        // same in-RAM table to both copies.
        self.alt_entries.copy_from_slice(&self.entries);

        let ecrc = crc32fast::hash(&self.entries);
        put32(&mut self.hdr, 88, ecrc);
        put32(&mut self.hdr, 16, 0);
        let hcrc = crc32fast::hash(&self.hdr[..self.hdr_sz as usize]);
        put32(&mut self.hdr, 16, hcrc);

        put32(&mut self.alt_hdr, 88, ecrc);
        put32(&mut self.alt_hdr, 16, 0);
        let hdr_sz = le32(&self.alt_hdr, 12) as usize;
        let hcrc = crc32fast::hash(&self.alt_hdr[..hdr_sz]);
        put32(&mut self.alt_hdr, 16, hcrc);
    }

    fn write<W: Write + Seek>(&self, w: &mut W) -> io::Result<()> {
        let bs = self.lba as u64;
        w.seek(SeekFrom::Start(self.entry_lba * bs))?;
        w.write_all(&self.entries)?;
        w.seek(SeekFrom::Start(bs))?;
        w.write_all(&self.hdr)?;
        w.seek(SeekFrom::Start(self.alt_entry_lba * bs))?;
        w.write_all(&self.alt_entries)?;
        w.seek(SeekFrom::Start(self.alt_lba * bs))?;
        w.write_all(&self.alt_hdr)?;
        w.flush()
    }
}

fn entry_name(e: &[u8]) -> String {
    let mut s = String::new();
    let mut i = 0;
    while i + 1 < PART_NAME_LEN {
        let c = u16::from_le_bytes([e[PART_NAME_OFF + i], e[PART_NAME_OFF + i + 1]]);
        if c == 0 {
            break;
        }
        s.push(char::from_u32(c as u32).unwrap_or('?'));
        i += 2;
    }
    s
}

/// Would this write leave *no* bootable slot? That is the state stock ABL
/// cannot recover from without fastboot, so it needs an explicit `--force`.
fn strands_device(other_unbootable: u64, new_unbootable: u64, force: bool) -> bool {
    !force && other_unbootable == 1 && new_unbootable == 1
}

fn attr_field(at: u64, shift: u32, mask: u64) -> u64 {
    (at >> shift) & mask
}

fn with_field(at: u64, shift: u32, mask: u64, v: u64) -> u64 {
    (at & !(mask << shift)) | ((v & mask) << shift)
}

// ---------------------------------------------------------------------------
// device discovery
// ---------------------------------------------------------------------------

/// `sda17` -> `sda`, `mmcblk0p12` -> `mmcblk0`, `nvme0n1p3` -> `nvme0n1`.
fn parent_disk(name: &str) -> String {
    let digits_at = name
        .char_indices()
        .rev()
        .take_while(|(_, c)| c.is_ascii_digit())
        .last()
        .map(|(i, _)| i)
        .unwrap_or(name.len());
    if digits_at == name.len() {
        return name.to_string(); // no partition suffix
    }
    let before = &name[..digits_at];
    if let Some(stripped) = before.strip_suffix('p') {
        // mmcblk0p12 / nvme0n1p3
        if before.starts_with("mmcblk") || before.starts_with("nvme") {
            return stripped.to_string();
        }
    }
    before.to_string()
}

fn disk_from_byname() -> Option<PathBuf> {
    let link = std::fs::read_link("/dev/block/by-name/boot_a").ok()?;
    let base = link.file_name()?.to_str()?;
    Some(PathBuf::from("/dev/block").join(parent_disk(base)))
}

fn probe_has_both(path: &Path, lba: Option<u32>) -> Option<Gpt> {
    let mut f = File::open(path).ok()?;
    Gpt::parse(&mut f, lba)
        .ok()
        .filter(|g| g.slot_index("a").is_some() && g.slot_index("b").is_some())
}

fn find_disk(opt: &DiskOpt, writable: bool) -> Result<(PathBuf, Gpt), String> {
    if let Some(p) = &opt.disk {
        let mut f = OpenOptions::new()
            .read(true)
            .write(writable)
            .open(p)
            .map_err(|e| format!("{}: {e}", p.display()))?;
        let g = Gpt::parse(&mut f, opt.lba)?;
        return Ok((p.clone(), g));
    }

    let mut candidates: Vec<PathBuf> = Vec::new();
    if let Some(p) = disk_from_byname() {
        candidates.push(p);
    }
    for prefix in ["sd", "mmcblk", "nvme", "ufs"] {
        if let Ok(rd) = std::fs::read_dir("/dev/block") {
            for de in rd.flatten() {
                let name = de.file_name().to_string_lossy().to_string();
                if !name.starts_with(prefix) {
                    continue;
                }
                // A GPT lives on the whole disk, never on a partition node.
                if Path::new(&format!("/sys/class/block/{name}/partition")).exists() {
                    continue;
                }
                let p = PathBuf::from("/dev/block").join(&name);
                if !candidates.contains(&p) {
                    candidates.push(p);
                }
            }
        }
    }

    for p in candidates {
        if let Some(g) = probe_has_both(&p, opt.lba) {
            if !writable {
                return Ok((p, g));
            }
            // Re-open read-write and re-parse so writes go to the same handle.
            let mut f = OpenOptions::new()
                .read(true)
                .write(true)
                .open(&p)
                .map_err(|e| format!("{}: {e} (root required?)", p.display()))?;
            let g = Gpt::parse(&mut f, Some(g.lba))?;
            return Ok((p, g));
        }
    }
    Err("no GPT with boot_a and boot_b found; pass --disk".into())
}

// ---------------------------------------------------------------------------
// remote sources
// ---------------------------------------------------------------------------

const DEVICE_BIN: &str = "/data/adb/modules/gbl-slotflag/bin/gbl";

/// `fastboot getvar` blocks forever when no device is attached, so probe with
/// `fastboot devices` (which returns immediately) before asking for a value.
fn fastboot_has_device(serial: Option<&str>) -> bool {
    let mut args: Vec<String> = Vec::new();
    if let Some(sn) = serial {
        args.push("-s".into());
        args.push(sn.to_string());
    }
    args.push("devices".into());
    match run_program("fastboot", &args) {
        Ok(out) => out.lines().any(|l| {
            let l = l.trim();
            !l.is_empty() && !l.starts_with("List of devices")
        }),
        Err(_) => false,
    }
}

fn adb_has_device(serial: Option<&str>) -> bool {
    let mut args: Vec<String> = Vec::new();
    if let Some(sn) = serial {
        args.push("-s".into());
        args.push(sn.to_string());
    }
    args.push("devices".into());
    match run_program("adb", &args) {
        Ok(out) => out
            .lines()
            .skip_while(|l| l.starts_with("List of devices"))
            .any(|l| l.contains("\tdevice")),
        Err(_) => false,
    }
}

/// Merge stdout+stderr: `fastboot getvar` writes its answers to stderr.
fn run_program(prog: &str, args: &[String]) -> Result<String, String> {
    let out = std::process::Command::new(prog)
        .args(args)
        .output()
        .map_err(|e| format!("{prog}: {e} (is it installed and on PATH?)"))?;
    let mut s = String::from_utf8_lossy(&out.stdout).into_owned();
    s.push_str(&String::from_utf8_lossy(&out.stderr));
    Ok(s)
}

/// Pull `<key>: <value>` out of a `fastboot getvar` transcript.
/// fastboot echoes `key: value`, may prefix it with `(bootloader) `, and
/// answers are not ordered.
fn parse_getvar(text: &str, key: &str) -> Option<String> {
    for raw in text.lines() {
        let mut line = raw.trim();
        if let Some(rest) = line.strip_prefix("(bootloader) ") {
            line = rest.trim();
        }
        if let Some(rest) = line.strip_prefix(key) {
            if let Some(v) = rest.strip_prefix(':') {
                return Some(v.trim().to_string());
            }
        }
    }
    None
}

fn yes_no(v: &str) -> Option<u8> {
    match v.trim() {
        "yes" | "1" | "true" => Some(1),
        "no" | "0" | "false" => Some(0),
        _ => None,
    }
}

fn show_via_fastboot(a: &ShowArgs) -> Result<(), GblError> {
    if !fastboot_has_device(a.disk.serial.as_deref()) {
        println!("error=no fastboot device");
        return Err(GblError::silent(1));
    }
    let mut base: Vec<String> = Vec::new();
    if let Some(sn) = &a.disk.serial {
        base.push("-s".into());
        base.push(sn.clone());
    }
    let getvar = |key: &str| -> Result<String, String> {
        let mut args = base.clone();
        args.push("getvar".into());
        args.push(key.into());
        let out = run_program("fastboot", &args)?;
        if out.contains("no devices") || out.contains("waiting for any device") {
            return Err("no fastboot device".into());
        }
        parse_getvar(&out, key).ok_or_else(|| format!("getvar {key}: no value in response"))
    };

    let current = getvar("current-slot").ok();
    println!("source=fastboot");
    if let Some(sn) = &a.disk.serial {
        println!("serial={sn}");
    }
    if let Some(c) = &current {
        println!("current-slot={c}");
    }

    let mut printed = false;
    for sfx in ["a", "b"] {
        let success = getvar(&format!("slot-successful:{sfx}")).ok();
        let unboot = getvar(&format!("slot-unbootable:{sfx}")).ok();
        let retry = getvar(&format!("slot-retry-count:{sfx}")).ok();
        if success.is_none() && unboot.is_none() && retry.is_none() {
            println!("{sfx}.missing=1");
            continue;
        }
        printed = true;
        if let Some(v) = success.as_deref().and_then(yes_no) {
            println!("{sfx}.successful={v}");
        }
        if let Some(v) = unboot.as_deref().and_then(yes_no) {
            println!("{sfx}.unbootable={v}");
        }
        if let Some(v) = retry.as_deref().and_then(|t| t.parse::<u8>().ok()) {
            println!("{sfx}.retry={v}");
        }
    }
    if !printed {
        println!("error=this fastboot does not publish slot attributes");
        return Err(GblError::silent(1));
    }
    println!("note=priority/active are not published by getvar; use --adb or the on-device binary");
    Ok(())
}

fn show_via_adb(a: &ShowArgs) -> Result<(), GblError> {
    if !adb_has_device(a.disk.serial.as_deref()) {
        println!("error=no adb device");
        return Err(GblError::silent(1));
    }
    let mut args: Vec<String> = Vec::new();
    if let Some(sn) = &a.disk.serial {
        args.push("-s".into());
        args.push(sn.clone());
    }
    args.push("shell".into());
    args.push(format!("su -c '{} slot show'", a.disk.device_bin));
    let out = run_program("adb", &args).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    let body: String = out
        .lines()
        .filter(|l| !l.starts_with("error="))
        .collect::<Vec<_>>()
        .join("\n");
    if !out.contains("crc=ok") && !out.contains(".attr=") {
        println!("error=on-device read failed");
        println!("{out}");
        return Err(GblError::silent(1));
    }
    println!("source=adb");
    if let Some(sn) = &a.disk.serial {
        println!("serial={sn}");
    }
    println!("{body}");
    Ok(())
}

// ---------------------------------------------------------------------------
// commands
// ---------------------------------------------------------------------------

fn print_slot(g: &Gpt, sfx: &str) {
    if let Some(i) = g.slot_index(sfx) {
        let at = g.attr(i);
        println!("{sfx}.attr=0x{at:016x}");
        println!("{sfx}.priority={}", attr_field(at, ATTR_PRIORITY_BIT, 3));
        println!("{sfx}.active={}", attr_field(at, ATTR_ACTIVE_BIT, 1));
        println!("{sfx}.retry={}", attr_field(at, ATTR_RETRY_BIT, 7));
        println!("{sfx}.successful={}", attr_field(at, ATTR_SUCCESS_BIT, 1));
        println!(
            "{sfx}.unbootable={}",
            attr_field(at, ATTR_UNBOOTABLE_BIT, 1)
        );
    }
}

fn cmd_show(a: ShowArgs) -> Result<(), GblError> {
    if a.disk.fastboot {
        return show_via_fastboot(&a);
    }
    if a.disk.adb {
        return show_via_adb(&a);
    }
    let (path, g) = match find_disk(&a.disk, false) {
        Ok(v) => v,
        Err(e) => {
            // On a host there is no /dev/block for the phone: fall back to
            // the read-only fastboot source instead of failing outright.
            if a.disk.disk.is_none() {
                println!("note=no local GPT ({e}); trying fastboot getvar");
                return show_via_fastboot(&a);
            }
            println!("error={e}");
            return Err(GblError::silent(1));
        }
    };
    println!("disk={}", path.display());
    println!("lba={}", g.lba);
    println!("entries={}", g.nent);
    println!("entry_size={}", g.esz);
    println!("crc=ok");
    print_slot(&g, "a");
    print_slot(&g, "b");
    Ok(())
}

fn out_of_range(name: &str, v: u8, max: u8) -> GblError {
    println!("error={name} must be 0..={max} (got {v})");
    GblError::silent(2)
}

fn cmd_set(a: SetArgs) -> Result<(), GblError> {
    if a.disk.adb {
        return set_via_adb(&a);
    }
    if a.disk.fastboot {
        println!("error=fastboot cannot write slot attributes; use --adb or run on the device");
        return Err(GblError::silent(2));
    }
    if a.slot != "a" && a.slot != "b" {
        println!("error=--slot a|b required");
        return Err(GblError::silent(2));
    }
    if a.unbootable.is_none()
        && a.successful.is_none()
        && a.active.is_none()
        && a.priority.is_none()
        && a.retry.is_none()
    {
        println!("error=nothing to change");
        return Err(GblError::silent(2));
    }
    if let Some(v) = a.unbootable {
        if v > 1 {
            return Err(out_of_range("--unbootable", v, 1));
        }
    }
    if let Some(v) = a.successful {
        if v > 1 {
            return Err(out_of_range("--successful", v, 1));
        }
    }
    if let Some(v) = a.active {
        if v > 1 {
            return Err(out_of_range("--active", v, 1));
        }
    }
    if let Some(v) = a.priority {
        if v > 3 {
            return Err(out_of_range("--priority", v, 3));
        }
    }
    if let Some(v) = a.retry {
        if v > 7 {
            return Err(out_of_range("--retry", v, 7));
        }
    }

    let (path, mut g) = match find_disk(&a.disk, !a.dry_run) {
        Ok(v) => v,
        Err(e) => {
            println!("error={e}");
            return Err(GblError::silent(1));
        }
    };
    let idx = match g.slot_index(&a.slot) {
        Some(i) => i,
        None => {
            println!("error=boot_{} not found in GPT", a.slot);
            return Err(GblError::silent(1));
        }
    };
    let other = if a.slot == "a" { "b" } else { "a" };
    let other_idx = match g.slot_index(other) {
        Some(i) => i,
        None => {
            println!("error=boot_{other} missing, refusing");
            return Err(GblError::silent(1));
        }
    };

    let old = g.attr(idx);
    let mut new = old;
    if let Some(v) = a.unbootable {
        new = with_field(new, ATTR_UNBOOTABLE_BIT, 1, v as u64);
    }
    if let Some(v) = a.successful {
        new = with_field(new, ATTR_SUCCESS_BIT, 1, v as u64);
    }
    if let Some(v) = a.active {
        new = with_field(new, ATTR_ACTIVE_BIT, 1, v as u64);
    }
    if let Some(v) = a.priority {
        new = with_field(new, ATTR_PRIORITY_BIT, 3, v as u64);
    }
    if let Some(v) = a.retry {
        new = with_field(new, ATTR_RETRY_BIT, 7, v as u64);
    }

    if new == old {
        println!("result=unchanged");
        println!("attr=0x{old:016x}");
        return Ok(());
    }

    let other_ub = attr_field(g.attr(other_idx), ATTR_UNBOOTABLE_BIT, 1);
    let new_ub = attr_field(new, ATTR_UNBOOTABLE_BIT, 1);
    if strands_device(other_ub, new_ub, a.force) {
        println!("error=both slots would be unbootable; pass --force");
        return Err(GblError::silent(3));
    }

    println!("old=0x{old:016x}");
    println!("new=0x{new:016x}");
    if a.dry_run {
        println!("result=dry-run");
        return Ok(());
    }

    g.set_attr(idx, new);
    g.reseal();
    let mut f = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&path)
        .map_err(|e| GblError::runtime(format!("{}: {e}", path.display())))?;
    println!("writing primary entries+header on {} ...", path.display());
    g.write(&mut f)
        .map_err(|e| GblError::runtime(format!("write failed: {e}")))?;
    f.sync_all()
        .map_err(|e| GblError::runtime(format!("sync failed: {e}")))?;
    println!("result=ok");
    Ok(())
}

const DUMP_MAGIC: &[u8; 10] = b"SLOTFLAG1\n";

fn set_via_adb(a: &SetArgs) -> Result<(), GblError> {
    if !adb_has_device(a.disk.serial.as_deref()) {
        println!("error=no adb device");
        return Err(GblError::silent(1));
    }
    if a.slot != "a" && a.slot != "b" {
        println!("error=--slot a|b required");
        return Err(GblError::silent(2));
    }
    let mut cmd = format!("{} slot set --slot {}", a.disk.device_bin, a.slot);
    if let Some(v) = a.unbootable {
        cmd += &format!(" --unbootable {v}");
    }
    if let Some(v) = a.successful {
        cmd += &format!(" --successful {v}");
    }
    if let Some(v) = a.active {
        cmd += &format!(" --active {v}");
    }
    if let Some(v) = a.priority {
        cmd += &format!(" --priority {v}");
    }
    if let Some(v) = a.retry {
        cmd += &format!(" --retry {v}");
    }
    if a.dry_run {
        cmd += " --dry-run";
    }
    if a.force {
        cmd += " --force";
    }

    let mut args: Vec<String> = Vec::new();
    if let Some(sn) = &a.disk.serial {
        args.push("-s".into());
        args.push(sn.clone());
    }
    args.push("shell".into());
    args.push(format!("su -c '{cmd}'"));
    let out = run_program("adb", &args).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    print!("{out}");
    if out.contains("result=ok")
        || out.contains("result=unchanged")
        || out.contains("result=dry-run")
    {
        Ok(())
    } else {
        println!("error=on-device write failed");
        Err(GblError::silent(1))
    }
}

fn backup_via_adb(a: &BackupArgs) -> Result<(), GblError> {
    if !adb_has_device(a.disk.serial.as_deref()) {
        println!("error=no adb device");
        return Err(GblError::silent(1));
    }
    let dev = "/data/local/tmp/gbl-slot-gpt.bin";
    let mut args: Vec<String> = Vec::new();
    if let Some(sn) = &a.disk.serial {
        args.push("-s".into());
        args.push(sn.clone());
    }
    args.push("shell".into());
    args.push(format!(
        "su -c '{} slot backup --out {dev} && chmod 644 {dev}'",
        a.disk.device_bin
    ));
    let out = run_program("adb", &args).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    if !out.contains("result=ok") {
        println!("error=on-device backup failed");
        print!("{out}");
        return Err(GblError::silent(1));
    }
    let mut pull: Vec<String> = Vec::new();
    if let Some(sn) = &a.disk.serial {
        pull.push("-s".into());
        pull.push(sn.clone());
    }
    pull.push("pull".into());
    pull.push(dev.into());
    pull.push(a.out.display().to_string());
    let out = run_program("adb", &pull).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    if !a.out.exists() {
        println!("error=adb pull failed");
        print!("{out}");
        return Err(GblError::silent(1));
    }
    let _ = run_program(
        "adb",
        &[
            vec!["shell".to_string()],
            vec![format!("su -c 'rm -f {dev}'")],
        ]
        .concat(),
    );
    println!("result=ok");
    println!("source=device");
    println!("path={}", a.out.display());
    Ok(())
}

fn cmd_backup(a: BackupArgs) -> Result<(), GblError> {
    if a.disk.adb {
        return backup_via_adb(&a);
    }
    if a.disk.fastboot {
        println!("error=fastboot cannot dump the GPT; use --adb");
        return Err(GblError::silent(2));
    }
    let (path, g) = match find_disk(&a.disk, false) {
        Ok(v) => v,
        Err(e) => {
            println!("error={e}");
            return Err(GblError::silent(1));
        }
    };
    let mut buf = Vec::new();
    buf.extend_from_slice(DUMP_MAGIC);
    buf.extend_from_slice(&g.lba.to_le_bytes());
    buf.extend_from_slice(&g.nent.to_le_bytes());
    buf.extend_from_slice(&g.esz.to_le_bytes());
    buf.extend_from_slice(&g.entry_lba.to_le_bytes());
    buf.extend_from_slice(&g.alt_lba.to_le_bytes());
    buf.extend_from_slice(&g.alt_entry_lba.to_le_bytes());
    buf.extend_from_slice(&g.hdr);
    buf.extend_from_slice(&g.entries);
    buf.extend_from_slice(&g.alt_hdr);
    buf.extend_from_slice(&g.alt_entries);
    std::fs::write(&a.out, &buf)
        .map_err(|e| GblError::runtime(format!("{}: {e}", a.out.display())))?;
    println!("result=ok");
    println!("path={}", a.out.display());
    println!("source={}", path.display());
    Ok(())
}

struct Dump {
    lba: u32,
    nent: u32,
    esz: u32,
    entry_lba: u64,
    alt_lba: u64,
    alt_entry_lba: u64,
    hdr: Vec<u8>,
    entries: Vec<u8>,
    alt_hdr: Vec<u8>,
    alt_entries: Vec<u8>,
}

fn read_dump(p: &Path) -> Result<Dump, GblError> {
    let b = std::fs::read(p).map_err(|e| GblError::runtime(format!("{}: {e}", p.display())))?;
    if b.len() < 10 + 4 + 4 + 4 + 8 + 8 + 8 || &b[..10] != DUMP_MAGIC {
        return Err(GblError::runtime("not a `gbl slot backup` dump"));
    }
    let lba = le32(&b, 10);
    let nent = le32(&b, 14);
    let esz = le32(&b, 18);
    let entry_lba = le64(&b, 22);
    let alt_lba = le64(&b, 30);
    let alt_entry_lba = le64(&b, 38);
    if !(512..=4096).contains(&lba) {
        return Err(GblError::runtime("dump has bad block size"));
    }
    let mut off = 46;
    let take = |off: &mut usize, n: usize| -> Result<Vec<u8>, GblError> {
        if *off + n > b.len() {
            return Err(GblError::runtime("truncated dump"));
        }
        let v = b[*off..*off + n].to_vec();
        *off += n;
        Ok(v)
    };
    let hdr = take(&mut off, lba as usize)?;
    let entries = take(&mut off, nent as usize * esz as usize)?;
    let alt_hdr = take(&mut off, lba as usize)?;
    let alt_entries = take(&mut off, nent as usize * esz as usize)?;
    Ok(Dump {
        lba,
        nent,
        esz,
        entry_lba,
        alt_lba,
        alt_entry_lba,
        hdr,
        entries,
        alt_hdr,
        alt_entries,
    })
}

fn restore_via_adb(a: &RestoreArgs) -> Result<(), GblError> {
    if !adb_has_device(a.disk.serial.as_deref()) {
        println!("error=no adb device");
        return Err(GblError::silent(1));
    }
    let dev = "/data/local/tmp/gbl-slot-gpt.bin";
    let push: Vec<String> = [
        a.disk
            .serial
            .as_ref()
            .map(|s| vec!["-s".to_string(), s.clone()])
            .unwrap_or_default(),
        vec![
            "push".to_string(),
            a.r#in.display().to_string(),
            dev.to_string(),
        ],
    ]
    .concat();
    let out = run_program("adb", &push).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    if !out.contains("1 file pushed") && !out.contains("pushed") {
        println!("error=adb push failed");
        print!("{out}");
        return Err(GblError::silent(1));
    }
    let shell: Vec<String> = [
        a.disk
            .serial
            .as_ref()
            .map(|s| vec!["-s".to_string(), s.clone()])
            .unwrap_or_default(),
        vec![
            "shell".to_string(),
            format!(
                "su -c '{} slot restore --in {dev}{} && rm -f {dev}'",
                a.disk.device_bin,
                if a.force { " --force" } else { "" }
            ),
        ],
    ]
    .concat();
    let out = run_program("adb", &shell).map_err(|e| {
        println!("error={e}");
        GblError::silent(1)
    })?;
    print!("{out}");
    if out.contains("result=ok") {
        println!("target=device");
        Ok(())
    } else {
        println!("error=on-device restore failed");
        Err(GblError::silent(1))
    }
}

fn cmd_restore(a: RestoreArgs) -> Result<(), GblError> {
    if a.disk.adb {
        return restore_via_adb(&a);
    }
    if a.disk.fastboot {
        println!("error=fastboot cannot write the GPT; use --adb");
        return Err(GblError::silent(2));
    }
    let d = read_dump(&a.r#in)?;
    let (path, g) = match find_disk(&a.disk, true) {
        Ok(v) => v,
        Err(e) => {
            println!("error={e}");
            return Err(GblError::silent(1));
        }
    };
    if !a.force && (d.nent != g.nent || d.esz != g.esz || d.lba != g.lba) {
        println!(
            "error=dump geometry {}x{}/{} != target {}x{}/{}; pass --force",
            d.nent, d.esz, d.lba, g.nent, g.esz, g.lba
        );
        return Err(GblError::silent(1));
    }
    let hdr_sz = le32(&d.hdr, 12);
    let out = Gpt {
        lba: d.lba,
        total_lba: g.total_lba,
        hdr: d.hdr,
        hdr_sz,
        alt_lba: d.alt_lba,
        entries: d.entries,
        entry_lba: d.entry_lba,
        alt_hdr: d.alt_hdr,
        alt_entry_lba: d.alt_entry_lba,
        alt_entries: d.alt_entries,
        nent: d.nent,
        esz: d.esz,
    };
    let mut f = OpenOptions::new()
        .read(true)
        .write(true)
        .open(&path)
        .map_err(|e| GblError::runtime(format!("{}: {e}", path.display())))?;
    out.write(&mut f)
        .map_err(|e| GblError::runtime(format!("write failed: {e}")))?;
    f.sync_all()
        .map_err(|e| GblError::runtime(format!("sync failed: {e}")))?;
    println!("result=ok");
    println!("source={}", a.r#in.display());
    println!("target={}", path.display());
    Ok(())
}

pub fn run(args: Args) -> Result<(), GblError> {
    match args.cmd {
        Cmd::Show(a) => cmd_show(a),
        Cmd::Set(a) => cmd_set(a),
        Cmd::Backup(a) => cmd_backup(a),
        Cmd::Restore(a) => cmd_restore(a),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Cursor;

    const NENT: u32 = 128;
    const ESZ: u32 = 128;

    fn entry(name: &str, first: u64, last: u64, attrs: u64) -> Vec<u8> {
        let mut e = vec![0u8; ESZ as usize];
        // Linux data GUID, lowest bytes non-zero so the entry counts as used.
        e[0] = 0xAF;
        put64(&mut e, 32, first);
        put64(&mut e, 40, last);
        put64(&mut e, PART_ATTR_OFF, attrs);
        for (i, c) in name.encode_utf16().take(36).enumerate() {
            e[PART_NAME_OFF + i * 2..PART_NAME_OFF + i * 2 + 2].copy_from_slice(&c.to_le_bytes());
        }
        e
    }

    fn seal(ents: &mut [u8], hdr: &mut [u8], hdr_sz: usize) -> u32 {
        let ecrc = crc32fast::hash(ents);
        put32(hdr, 88, ecrc);
        put32(hdr, 16, 0);
        let hcrc = crc32fast::hash(&hdr[..hdr_sz]);
        put32(hdr, 16, hcrc);
        ecrc
    }

    fn build_image(lba: u32) -> Vec<u8> {
        let total_lba: u64 = (16 * 1024 * 1024) / lba as u64;
        let mut img = vec![0u8; total_lba as usize * lba as usize];

        let mut ents = vec![0u8; NENT as usize * ESZ as usize];
        let a = entry(
            "boot_a",
            40,
            103,
            (1 << ATTR_PRIORITY_BIT)
                | (1 << ATTR_ACTIVE_BIT)
                | (7 << ATTR_RETRY_BIT)
                | (1 << ATTR_SUCCESS_BIT),
        );
        let b = entry("boot_b", 104, 167, 1 << ATTR_UNBOOTABLE_BIT);
        let s = entry("system_a", 168, 231, 0);
        ents[..ESZ as usize].copy_from_slice(&a);
        ents[ESZ as usize..2 * ESZ as usize].copy_from_slice(&b);
        ents[2 * ESZ as usize..3 * ESZ as usize].copy_from_slice(&s);

        let backup_lba = total_lba - 1;
        let backup_entry_lba = backup_lba - (NENT as u64 * ESZ as u64) / lba as u64;

        let mk_hdr = |my: u64, alt: u64, el: u64, ecrc: u32| {
            let mut h = vec![0u8; lba as usize];
            h[..8].copy_from_slice(b"EFI PART");
            put32(&mut h, 8, 0x0001_0000);
            put32(&mut h, 12, 92);
            put64(&mut h, 24, my);
            put64(&mut h, 32, alt);
            put64(&mut h, 40, 34);
            put64(&mut h, 48, total_lba - 34);
            put64(&mut h, 72, el);
            put32(&mut h, 80, NENT);
            put32(&mut h, 84, ESZ);
            put32(&mut h, 88, ecrc);
            let mut t = h[..92].to_vec();
            put32(&mut t, 16, 0);
            put32(&mut h, 16, crc32fast::hash(&t));
            h
        };

        let ecrc = crc32fast::hash(&ents);
        let prim = mk_hdr(1, backup_lba, 2, ecrc);
        let bak = mk_hdr(backup_lba, 1, backup_entry_lba, ecrc);

        let put = |img: &mut Vec<u8>, at: u64, data: &[u8]| {
            let o = at as usize * lba as usize;
            img[o..o + data.len()].copy_from_slice(data);
        };
        put(&mut img, 1, &prim);
        put(&mut img, 2, &ents);
        put(&mut img, backup_entry_lba, &ents);
        put(&mut img, backup_lba, &bak);
        img
    }

    fn parse_img(img: &[u8]) -> Gpt {
        let mut c = Cursor::new(img.to_vec());
        Gpt::parse(&mut c, None).expect("synthetic image must parse")
    }

    #[test]
    fn reads_attributes_at_512_and_4096() {
        for lba in [512u32, 4096] {
            let img = build_image(lba);
            let g = parse_img(&img);
            assert_eq!(g.lba, lba);
            let a = g.attr(g.slot_index("a").unwrap());
            assert_eq!(attr_field(a, ATTR_PRIORITY_BIT, 3), 1);
            assert_eq!(attr_field(a, ATTR_ACTIVE_BIT, 1), 1);
            assert_eq!(attr_field(a, ATTR_RETRY_BIT, 7), 7);
            assert_eq!(attr_field(a, ATTR_SUCCESS_BIT, 1), 1);
            assert_eq!(attr_field(a, ATTR_UNBOOTABLE_BIT, 1), 0);
            let b = g.attr(g.slot_index("b").unwrap());
            assert_eq!(attr_field(b, ATTR_UNBOOTABLE_BIT, 1), 1);
        }
    }

    #[test]
    fn rewrite_reseals_both_copies() {
        let img = build_image(512);
        let mut g = parse_img(&img);
        let idx = g.slot_index("b").unwrap();
        let new = with_field(g.attr(idx), ATTR_UNBOOTABLE_BIT, 1, 0);
        g.set_attr(idx, new);
        g.reseal();
        assert_eq!(g.entries, g.alt_entries);

        let mut out = Cursor::new(Vec::new());
        g.write(&mut out).expect("write");
        let img2 = out.into_inner();

        // Re-parsing re-validates both entry CRCs and both header CRCs.
        let g2 = parse_img(&img2);
        assert_eq!(
            attr_field(g2.attr(g2.slot_index("b").unwrap()), ATTR_UNBOOTABLE_BIT, 1),
            0
        );
        assert_eq!(
            attr_field(g2.attr(g2.slot_index("a").unwrap()), ATTR_PRIORITY_BIT, 3),
            1
        );
        assert_eq!(g2.entries, g2.alt_entries);
    }

    #[test]
    fn corrupt_entry_table_is_rejected() {
        let mut img = build_image(512);
        let off = 2 * 512 + 8;
        img[off] ^= 0xFF;
        let mut c = Cursor::new(img);
        let err = Gpt::parse(&mut c, None).unwrap_err();
        assert!(err.contains("entry array CRC"), "got {err}");
    }

    #[test]
    fn corrupt_backup_header_is_rejected() {
        let lba = 512usize;
        let mut img = build_image(512);
        let total_lba = img.len() / lba;
        let off = (total_lba - 1) * lba + 8;
        img[off] ^= 0xFF;
        let mut c = Cursor::new(img);
        let err = Gpt::parse(&mut c, None).unwrap_err();
        assert!(err.contains("backup header"), "got {err}");
    }

    #[test]
    fn both_unbootable_needs_force() {
        assert!(strands_device(1, 1, false));
        assert!(!strands_device(1, 1, true));
        assert!(!strands_device(0, 1, false));
        assert!(!strands_device(1, 0, false));
    }

    #[test]
    fn field_edits_are_masked() {
        let base = 0u64;
        let v = with_field(base, ATTR_RETRY_BIT, 7, 5);
        assert_eq!(attr_field(v, ATTR_RETRY_BIT, 7), 5);
        let v = with_field(v, ATTR_RETRY_BIT, 7, 0);
        assert_eq!(attr_field(v, ATTR_RETRY_BIT, 7), 0);
        // Neighbouring fields must survive.
        let v = with_field(0xFFu64 << 40, ATTR_PRIORITY_BIT, 3, 2);
        assert_eq!(attr_field(v, ATTR_PRIORITY_BIT, 3), 2);
        assert_eq!((v >> 40) & 0xFF, 0xFF);
    }

    #[test]
    fn parent_disk_strips_partition_suffix() {
        assert_eq!(parent_disk("sda17"), "sda");
        assert_eq!(parent_disk("sda"), "sda");
        assert_eq!(parent_disk("mmcblk0p12"), "mmcblk0");
        assert_eq!(parent_disk("nvme0n1p3"), "nvme0n1");
        assert_eq!(parent_disk("sdb"), "sdb");
    }

    #[test]
    fn getvar_parsing_handles_prefix_and_colons() {
        let t = "(bootloader) slot-unbootable:b: yes\nFinished. Total time: 0.001s\n";
        assert_eq!(parse_getvar(t, "slot-unbootable:b").as_deref(), Some("yes"));
        let t2 = "slot-retry-count:a: 7\r\n";
        assert_eq!(parse_getvar(t2, "slot-retry-count:a").as_deref(), Some("7"));
        // A key must match as a whole prefix, not as a substring of another.
        assert_eq!(
            parse_getvar("slot-unbootable:b: yes\n", "slot-unbootable:a"),
            None
        );
        assert_eq!(parse_getvar("nothing here\n", "current-slot"), None);
    }

    #[test]
    fn yes_no_maps_boolean_spellings() {
        assert_eq!(yes_no("yes"), Some(1));
        assert_eq!(yes_no("no"), Some(0));
        assert_eq!(yes_no("1"), Some(1));
        assert_eq!(yes_no("0"), Some(0));
        assert_eq!(yes_no("maybe"), None);
    }

    #[test]
    fn entry_name_decodes_utf16() {
        let e = entry("boot_b", 0, 0, 0);
        assert_eq!(entry_name(&e), "boot_b");
    }
}
