#!/usr/bin/env bash
# msc_selftest.sh - automated checks for the read-only ISO/file MSC LUNs and
#                   the MODE SENSE implementation.
#
# Two phases, because the phone has to move between Android and GBL Fastboot:
#
#   tests/device/msc_selftest.sh prepare   # Android: record an ESP file (read-only)
#   #  ... enter GBL Fastboot (hold Volume Up during the boot window) ...
#   tests/device/msc_selftest.sh run       # GBL Fastboot: export + verify
#
# `prepare` writes nothing to the device unless `--write` is given. `run`
# stages nothing and flashes nothing. It only issues
# `fastboot oem usb-msc ...`, reads the host block devices and ejects, so the
# phone returns to the same Fastboot session.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

FASTBOOT=${MSC_FASTBOOT_BIN:-fastboot}
ADB=${MSC_ADB_BIN:-adb}
SUDO=${MSC_SUDO_BIN:-sudo}
PYTHON=${MSC_PYTHON_BIN:-python3}
SYSFS=${MSC_SYSFS_ROOT:-/sys}
DEV_PREFIX=${MSC_DEV_PREFIX:-/dev/}
STATE_DIR=${MSC_STATE_DIR:-local}
STATE_FILE=$STATE_DIR/msc-selftest.env
PROBE=$ROOT/tests/device/msc_probe.py

DRY_RUN=0
KEEP_MSC=0
CASES=${MSC_CASES:-"file iso mixed"}
LUN_TIMEOUT=${MSC_LUN_TIMEOUT:-30}
TEST_SIZE=$((5 * 1024 * 1024 + 1234))   # not a multiple of 512 or 2048
FW_DIR=EFI/GBLMSC
FW_FILE=$FW_DIR/selftest.bin

FAILURES=0
CHECKS=0
SKIPS=0

say() { printf '%s\n' "$*"; }
note() { printf '  %s\n' "$*"; }
pass() { CHECKS=$((CHECKS + 1)); printf 'PASS  %s\n' "$*"; }
fail() { FAILURES=$((FAILURES + 1)); printf 'FAIL  %s\n' "$*"; }
skip() { SKIPS=$((SKIPS + 1)); printf 'SKIP  %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 2; }

usage() {
  cat <<'EOF'
usage: tests/device/msc_selftest.sh <command> [options]

  prepare [--from <fw-path>] [--mount <mountpoint>]
          Android phase (needs adb + root). Read-only: locates an existing
          file on a mounted FAT volume, records its size and SHA-256 in
          local/msc-selftest.env. Nothing is written to the device.
          Default --from /EFI/BOOT/BOOTAA64.EFI.

  prepare --write [--size <bytes>]
          Opt-in: generates a deterministic test file and copies it to
          <volume>/EFI/GBLMSC/selftest.bin. This modifies the device.

  run [--dry-run] [--keep] [--cases "<list>"]
          GBL Fastboot phase. For each case it runs `oem usb-msc`, verifies
          the host devices (logical block size, read-only flag, capacity,
          content hash, write rejection, INQUIRY, MODE SENSE, and MMC
          GET CONFIGURATION / READ TOC for the ISO LUN), then ejects so the
          phone returns to Fastboot. Cases: file iso mixed.

  status          Show the test file record and any GBL LUNs visible now.
  volumes         List the vfat volumes Android has mounted, with their
                  top-level entries (read-only: `mount` + `ls`).
  eject           Eject a LUN left exported by an earlier `oem usb-msc` and
                  wait for the Fastboot interface to come back.
  list-disks      Print "<dev>|<model>|<block>|<ro>|<sectors>" for GBL LUNs.
  self-test       Offline checks of tests/device/msc_probe.py.

Environment overrides (also used by tests/host/105_msc_selftest.sh):
  MSC_FASTBOOT_BIN MSC_ADB_BIN MSC_SUDO_BIN MSC_PYTHON_BIN MSC_SYSFS_ROOT
  MSC_STATE_DIR MSC_CASES MSC_DEV_PREFIX (test hooks)
EOF
}

on_dev() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '  DRY:'
    printf ' %q' "$@"
    printf '\n'
    return 0
  fi
  "$@"
}

# This GBL Fastboot re-arms its USB interface between sessions: right after one
# command the next `fastboot` call can return "no device" for a few seconds.
# Each helper therefore uses a generous per-attempt timeout, retries, and keeps
# the answer, so a normal run costs two sessions instead of a dozen.
FB_TIMEOUT=${MSC_FASTBOOT_TIMEOUT:-15}
FB_ATTEMPTS=${MSC_FASTBOOT_ATTEMPTS:-3}

note_err() { printf '  %s\n' "$*" >&2; }

fb_getvar() { # <name> [<attempts>]
  local name=$1 attempts=${2:-$FB_ATTEMPTS} out try=1
  while :; do
    # fastboot prints getvar results on stderr, so both streams are merged;
    # discarding stderr here silently turned every probe into "not in Fastboot".
    out=$(timeout "$FB_TIMEOUT" "$FASTBOOT" getvar "$name" 2>&1 \
          | sed -n "s/^$name:[[:space:]]*//p" | head -n1 || true)
    if [ -n "$out" ]; then
      printf '%s\n' "$out"
      return 0
    fi
    [ "$try" -ge "$attempts" ] && return 1
    try=$((try + 1))
    note_err "no answer yet, retrying ($try/$attempts)"
    sleep 4
  done
}

GBL_BUILD=""

gbl_build() { # cached build id; empty when the Fastboot interface is absent
  if [ -z "$GBL_BUILD" ]; then
    GBL_BUILD=$(fb_getvar gbl-chainload_build 2 || true)
  fi
  printf '%s' "$GBL_BUILD"
}

forget_build() {
  GBL_BUILD=""
}

in_gbl_fastboot() {
  [ "$DRY_RUN" = 1 ] && return 0
  [ -n "$(gbl_build)" ]
}

lun_rows() {
  local entry vendor model name
  # A CD-ROM (ISO) LUN is bound by sr_mod and shows up as /dev/sr*, every other
  # LUN as /dev/sd*.
  for entry in "$SYSFS"/block/sd* "$SYSFS"/block/sr*; do
    [ -r "$entry/device/model" ] || continue
    vendor=$(tr -d ' \n' <"$entry/device/vendor" 2>/dev/null || true)
    [ "$vendor" = "GBL" ] || continue
    model=$(tr -d '\n' <"$entry/device/model" | sed -e 's/[[:space:]]*$//')
    name=$(basename "$entry")
    printf '%s|%s|%s|%s|%s\n' "$name" "$model" \
      "$(cat "$entry/queue/logical_block_size" 2>/dev/null || echo 0)" \
      "$(cat "$entry/ro" 2>/dev/null || echo 0)" \
      "$(cat "$entry/size" 2>/dev/null || echo 0)"
  done
}

count_rows() {
  local rows=$1
  if [ -z "$rows" ]; then echo 0; else printf '%s\n' "$rows" | grep -c . ; fi
}

wait_for_luns() { # <count> <timeout-seconds>
  local want=$1 timeout=$2 waited=0 rows
  while :; do
    rows=$(lun_rows)
    if [ "$(count_rows "$rows")" -ge "$want" ]; then
      printf '%s\n' "$rows"
      return 0
    fi
    if [ "$waited" -ge "$timeout" ]; then
      printf '%s\n' "$rows"
      return 1
    fi
    sleep 1
    waited=$((waited + 1))
  done
}

wait_for_no_luns() { # <timeout-seconds>
  local timeout=$1 waited=0
  while :; do
    [ "$(count_rows "$(lun_rows)")" -eq 0 ] && return 0
    [ "$waited" -ge "$timeout" ] && return 1
    sleep 1
    waited=$((waited + 1))
  done
}

wait_for_fastboot() { # <timeout-seconds>; one probe per round, cache refreshed
  local timeout=$1 waited=0 out
  forget_build
  while :; do
    out=$(fb_getvar gbl-chainload_build 1 || true)
    if [ -n "$out" ]; then
      GBL_BUILD=$out
      return 0
    fi
    [ "$waited" -ge "$timeout" ] && return 1
    sleep 4
    waited=$((waited + 4))
  done
}

row_field() { printf '%s' "$1" | cut -d'|' -f"$2"; }

lun_for_model() { # <rows> <model>
  printf '%s\n' "$1" | awk -F'|' -v want="$2" '$2 == want { print; exit }'
}

hash_head() { # <dev> <bytes> — stream the first <bytes> of the LUN
  local dev=$DEV_PREFIX$1 bytes=$2 full rest
  full=$(( bytes / 1048576 ))
  rest=$(( bytes % 1048576 ))
  {
    if [ "$full" -gt 0 ]; then
      $SUDO dd if="$dev" bs=1048576 count="$full" status=none || true
    fi
    if [ "$rest" -gt 0 ]; then
      $SUDO dd if="$dev" bs="$rest" count=1 skip=$((full * 1048576)) \
        iflag=skip_bytes status=none || true
    fi
  } | sha256sum | cut -d' ' -f1
}

write_probe() { # <dev> <block-size> ; true when the content is unchanged
  local dev=$1 blk=$2 before after
  before=$(hash_head "$dev" "$blk")
  $SUDO dd if=/dev/zero of="$DEV_PREFIX$dev" bs="$blk" count=1 oflag=direct \
    conv=notrunc status=none 2>/dev/null || true
  after=$(hash_head "$dev" "$blk")
  [ "$before" = "$after" ]
}

probe() {
  if [ "$DRY_RUN" = 1 ]; then
    printf '  DRY: %q %q %q %q\n' "$SUDO" "$PYTHON" "$PROBE" "$1"
    return 0
  fi
  $SUDO "$PYTHON" "$PROBE" "$@"
}

round_up() { # <value> <multiple>
  echo $(( ($1 + $2 - 1) / $2 * $2 ))
}

# --------------------------------------------------------------------------
# prepare (Android side)
# --------------------------------------------------------------------------

vfat_mounts() {
  $ADB shell su -c mount 2>/dev/null \
    | sed -n 's/.* on \(.*\) type vfat .*/\1/p' | sort -u
}

generate_image() { # <host-path>
  "$PYTHON" - "$1" "$TEST_SIZE" <<'PY'
import hashlib
import sys

path = sys.argv[1]
size = int(sys.argv[2])
digest = hashlib.sha256()
header = b"GBLMSC-SELFTEST v1\n"
pattern = bytes(((i * 37 + 11) & 0xFF) for i in range(65536))
with open(path, "wb") as handle:
    handle.write(header)
    digest.update(header)
    remaining = size - len(header)
    while remaining > 0:
        chunk = pattern[:min(remaining, len(pattern))]
        handle.write(chunk)
        digest.update(chunk)
        remaining -= len(chunk)
print("%d %s" % (size, digest.hexdigest()))
PY
}

remote_stat_size() { # <device-path>; empty when the file is missing
  { $ADB shell su -c "stat -c %s '$1'" 2>/dev/null || true; } | tr -dc '0-9'
}

remote_sha256() { # <device-path>; empty when hashing failed
  { $ADB shell su -c "sha256sum '$1'" 2>/dev/null || true; } | cut -d' ' -f1 | tr -dc '0-9a-f'
}

cmd_prepare() {
  local from_path=/EFI/BOOT/BOOTAA64.EFI mount_override="" write_mode=0 size_given=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --from|--path) from_path=$2; shift 2 ;;
      --mount) mount_override=$2; shift 2 ;;
      --write) write_mode=1; shift ;;
      --size) TEST_SIZE=$2; size_given=1; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown prepare option: $1" ;;
    esac
  done
  if [ "$size_given" = 1 ] && [ "$write_mode" = 0 ]; then
    die "--size only applies to --write; the read-only flow measures the file it finds"
  fi
  command -v "$ADB" >/dev/null || die "adb not found"

  local uid mounts mount="" size="" hash=""
  uid=$($ADB shell su -c id 2>/dev/null || true)
  case "$uid" in
    *uid=0*) ;;
    *) die "adb root/su is required to read the vfat volume" ;;
  esac

  mounts=$(vfat_mounts)
  [ -n "$mounts" ] || die "no vfat volume is mounted on the device; the ESP is normally mounted only while Android runs"
  note "vfat volumes: $(printf '%s' "$mounts" | tr '\n' ' ')"

  if [ "$write_mode" = 1 ]; then
    for mount in $mounts; do
      if $ADB shell su -c "mkdir -p '$mount/$FW_DIR'" >/dev/null 2>&1 \
         && $ADB shell su -c "touch '$mount/$FW_DIR/.probe'" >/dev/null 2>&1; then
        break
      fi
      mount=""
    done
    [ -n "$mount" ] || die "no writable vfat volume; place a file another way and use --from"
    local host_image generated device_hash
    host_image=$(mktemp -t gblmsc.XXXXXX)
    generated=$(generate_image "$host_image")
    size=${generated%% *}
    hash=${generated##* }
    note "--write: this writes $mount/$FW_FILE ($size bytes) on the device"
    $ADB push "$host_image" "/sdcard/gblmsc-selftest.bin" >/dev/null
    $ADB shell su -c "cp '/sdcard/gblmsc-selftest.bin' '$mount/$FW_FILE'" >/dev/null
    $ADB shell su -c "rm -f '/sdcard/gblmsc-selftest.bin' '$mount/$FW_DIR/.probe'" >/dev/null 2>&1 || true
    $ADB shell su -c sync >/dev/null 2>&1 || true
    rm -f "$host_image"
    device_hash=$(remote_sha256 "$mount/$FW_FILE")
    [ "$device_hash" = "$hash" ] || die "pushed file hash mismatch ($device_hash != $hash)"
    from_path=/$FW_FILE
  else
    local fw_path=${from_path#/} candidate csize
    case "$fw_path" in
      *..*) die "refusing a path containing '..': $from_path" ;;
    esac
    if [ -n "$mount_override" ]; then
      mount=$mount_override
      size=$(remote_stat_size "$mount/$fw_path")
      [ -n "$size" ] || die "$mount/$fw_path is not readable on the device"
    else
      for candidate in $mounts; do
        csize=$(remote_stat_size "$candidate/$fw_path")
        if [ -n "$csize" ]; then
          mount=$candidate
          size=$csize
          break
        fi
      done
      [ -n "$mount" ] || die "$from_path was not found on any vfat volume; run '$(basename "$0") volumes' to see what Android has mounted, then pass --from/--mount"
    fi
    [ "$size" -gt 0 ] || die "$mount/$fw_path is empty"
    hash=$(remote_sha256 "$mount/$fw_path")
    [ -n "$hash" ] || note "the file could not be hashed on the device; the content check will be skipped"
    note "read-only: recorded $mount/$fw_path, nothing was written to the device"
    if [ $((size % 2048)) -eq 0 ]; then
      note "note: the file is a multiple of 2048 bytes, so the padding path is not exercised"
    fi
  fi

  mkdir -p "$STATE_DIR"
  {
    echo "# generated by tests/device/msc_selftest.sh prepare"
    echo "MSC_PATH=$from_path"
    echo "MSC_SIZE=${size:-}"
    echo "MSC_SHA256=${hash:-}"
    echo "MSC_VOLUME=${mount:-}"
  } >"$STATE_FILE"
  say "state written to $STATE_FILE"
  cat "$STATE_FILE"
}

# --------------------------------------------------------------------------
# run (GBL Fastboot side)
# --------------------------------------------------------------------------

load_state() {
  [ -f "$STATE_FILE" ] || die "missing $STATE_FILE; run 'prepare' first (or 'prepare --path ...')"
  # shellcheck disable=SC1090
  . "$STATE_FILE"
  [ -n "${MSC_PATH:-}" ] || die "$STATE_FILE has no MSC_PATH"
  case "${MSC_SIZE:-}" in ''|*[!0-9]*) MSC_SIZE="" ;; esac
}

verify_file_lun() { # <row> <expected-block-size> <expected-type> <label>
  local row=$1 block=$2 ptype=$3 label=$4
  local dev model lb ro sec out
  dev=$(row_field "$row" 1); model=$(row_field "$row" 2)
  lb=$(row_field "$row" 3); ro=$(row_field "$row" 4); sec=$(row_field "$row" 5)

  say "-- $label: /dev/$dev ($model)"
  if [ "$lb" = "$block" ]; then
    pass "$label logical_block_size=$lb"
  elif [ "$ptype" = "5" ]; then
    # /dev/sr* exposes the host's CD sector size here, not the LUN block size;
    # the device answer is checked through READ CAPACITY below.
    note "$label sysfs logical_block_size=$lb (host side, CD-ROM LUN)"
  else
    fail "$label logical_block_size=$lb (expected $block)"
  fi
  if [ "$ro" = "1" ]; then
    pass "$label read-only flag"
  else
    fail "$label read-only flag is $ro"
  fi

  if [ -n "${MSC_SIZE:-}" ]; then
    # sysfs "size" is always in 512-byte sectors, independent of the LUN's
    # logical block size.
    local want=$(( $(round_up "$MSC_SIZE" "$block") / 512 ))
    if [ "$sec" = "$want" ]; then
      pass "$label capacity $((sec * 512)) bytes, $((sec / (block / 512))) blocks of $block (padded from $MSC_SIZE)"
    else
      fail "$label capacity $sec sectors (expected $want)"
    fi
    if [ -n "${MSC_SHA256:-}" ]; then
      if [ "$DRY_RUN" = 1 ]; then
        note "DRY: hash /dev/$dev first $MSC_SIZE bytes"
      else
        local got
        got=$(hash_head "$dev" "$MSC_SIZE")
        if [ "$got" = "$MSC_SHA256" ]; then
          pass "$label content matches the source file"
        else
          fail "$label content hash $got != $MSC_SHA256"
        fi
      fi
    else
      skip "$label content hash (no reference hash recorded)"
    fi
  fi

  out=$(probe info "$DEV_PREFIX$dev" || true)
  if printf '%s' "$out" | grep -qx "inquiry.status=ok"; then
    local got_type
    got_type=$(printf '%s' "$out" | sed -n 's/^inquiry.type=//p')
    if [ "$got_type" = "$ptype" ]; then
      pass "$label INQUIRY peripheral type $got_type"
    else
      fail "$label INQUIRY peripheral type $got_type (expected $ptype)"
    fi
    printf '%s\n' "$out" | sed -n 's/^inquiry.product=/         product=/p'
    printf '%s\n' "$out" | sed -n 's/^inquiry.revision=/         product revision=/p'
    printf '%s\n' "$out" | sed -n 's/^capacity.block_size=/         capacity.block_size=/p'
    local cap_block
    cap_block=$(printf '%s' "$out" | sed -n 's/^capacity.block_size=//p')
    if [ "$cap_block" = "$block" ]; then
      pass "$label READ CAPACITY block size $cap_block"
    else
      fail "$label READ CAPACITY block size ${cap_block:-missing} (expected $block)"
    fi
    local revision
    revision=$(printf '%s' "$out" | sed -n 's/^inquiry.revision=//p')
    # Build marker: bump it whenever the on-wire behaviour changes, so a run
    # can be tied to a specific EFISP. 0001 lacks the MODE SENSE/page refusals,
    # 0002/0003 were intermediate builds, 0004 added the short-packet data
    # stage, 0005/0006 were diagnostic builds, and 0007 returns a real READ TOC
    # lead-out so the host sizes a CD-ROM LUN correctly.
    case "$revision" in
      0001|0002|0003|0004|0005|0006)
        note "product revision ${revision:-none}: predates the READ TOC lead-out fix"
        note "(0007); a CD-ROM LUN will show a wrong capacity; stage a newer EFISP" ;;
    esac
  else
    fail "$label INQUIRY failed: $(printf '%s' "$out" | tr '\n' ' ')"
  fi

  out=$(probe modesense "$DEV_PREFIX$dev" || true)
  if printf '%s' "$out" | grep -qx "mode6.status=ok"; then
    local wp lbd pages d0lbd d0bll
    printf '%s\n' "$out" | grep -E '^(mode|bad)' | sed 's/^/         /'
    wp=$(printf '%s' "$out" | sed -n 's/^mode6\.wp=//p')
    lbd=$(printf '%s' "$out" | sed -n 's/^mode6\.block_descriptor_length=//p')
    pages=$(printf '%s' "$out" | sed -n 's/^mode6\.pages=//p')
    d0lbd=$(printf '%s' "$out" | sed -n 's/^mode6_dbd0\.block_descriptor_length=//p')
    d0bll=$(printf '%s' "$out" | sed -n 's/^mode6_dbd0\.logical_block_length=//p')
    [ "$wp" = "1" ] && pass "$label MODE SENSE WP bit" || fail "$label MODE SENSE WP bit is $wp"
    # The default probe request sets DBD=1, so the descriptor must be absent.
    [ "$lbd" = "0" ] && pass "$label MODE SENSE omits the block descriptor under DBD=1" \
      || fail "$label MODE SENSE block descriptor under DBD=1 is $lbd (expected 0)"
    [ "$d0lbd" = "8" ] && pass "$label MODE SENSE block descriptor under DBD=0" \
      || fail "$label MODE SENSE block descriptor under DBD=0 is ${d0lbd:-missing} (expected 8)"
    [ "$d0bll" = "$block" ] && pass "$label MODE SENSE block length $d0bll" \
      || fail "$label MODE SENSE block length ${d0bll:-missing} (expected $block)"
    [ "$pages" = "08,0a" ] && pass "$label MODE SENSE pages $pages" \
      || fail "$label MODE SENSE pages ${pages:-none} (expected 08,0a)"
    printf '%s' "$out" | grep -qx "mode10.status=ok" \
      && pass "$label MODE SENSE(10)" || fail "$label MODE SENSE(10) failed"
    printf '%s' "$out" | grep -qx "mode6_pc1.status=check_condition" \
      && pass "$label changeable page control rejected" || fail "$label changeable page control not rejected"
    printf '%s' "$out" | grep -qx "mode6_bad_page.status=check_condition" \
      && pass "$label unknown page code rejected" || fail "$label unknown page code not rejected"
    printf '%s' "$out" | grep -qx "badread.status=check_condition" \
      && pass "$label read past the end rejected" || fail "$label read past the end not rejected"
    printf '%s' "$out" | grep -qx "badop.status=check_condition" \
      && pass "$label unsupported opcode rejected" || fail "$label unsupported opcode not rejected"
  else
    fail "$label MODE SENSE failed: $(printf '%s' "$out" | tr '\n' ' ')"
  fi

  if [ "$ptype" = "5" ]; then
    out=$(probe mmc "$DEV_PREFIX$dev" || true)
    printf '%s' "$out" | grep -qx "configuration.status=ok" \
      && pass "$label GET CONFIGURATION" || fail "$label GET CONFIGURATION failed"
    printf '%s' "$out" | grep -qx "configuration.current_profile=0008" \
      && pass "$label current profile is CD-ROM" \
      || fail "$label current profile $(printf '%s' "$out" | sed -n 's/^configuration.current_profile=//p')"
    printf '%s' "$out" | grep -qx "toc.status=ok" \
      && pass "$label READ TOC" || fail "$label READ TOC failed"
    printf '%s' "$out" | grep -qx "toc.track=1:4:0" \
      && pass "$label one data track at LBA 0" || fail "$label unexpected track list"
  fi

  out=$(probe openrw "$DEV_PREFIX$dev" || true)
  case "$out" in
    *refused*) pass "$label O_RDWR open refused ($(printf '%s' "$out" | cut -d= -f2))" ;;
    *) fail "$label O_RDWR open was allowed" ;;
  esac
}

# An export that does not surface the expected block devices has three possible
# causes that look identical from `lsblk`: the LUN was never scanned, no block
# driver bound to it (a CD-ROM LUN that sr_mod rejects), or the whole USB link
# is still in Fastboot. Print enough to tell them apart in one run.
describe_luns() { # <start-epoch>
  local started=$1 entry type model out
  if [ -d "$SYSFS/bus/scsi/devices" ]; then
    say "-- SCSI devices present now"
    for entry in "$SYSFS"/bus/scsi/devices/*; do
      [ -d "$entry" ] || continue
      type=$(cat "$entry/type" 2>/dev/null || echo '?')
      model=$(tr -d '\n' <"$entry/vendor" 2>/dev/null || true)
      model="$model $(tr -d '\n' <"$entry/model" 2>/dev/null || true)"
      printf '     %s type=%s %s\n' "$(basename "$entry")" "$type" \
        "$(printf '%s' "$model" | sed -e 's/[[:space:]]\+/ /g' -e 's/^ //' -e 's/ $//')"
    done
  fi
  say "-- block devices present now"
  { lsblk -o NAME,SIZE,RO,TYPE,MODEL 2>/dev/null || true; } | sed 's/^/     /'
  command -v journalctl >/dev/null 2>&1 || return 0
  out=$($SUDO journalctl -k --no-pager -o cat --since "@$started" 2>/dev/null \
        | grep -Ei "usb-storage|sr [0-9]+:|sd [0-9]+:|scsi [0-9]+:" | tail -20 || true)
  [ -n "$out" ] || return 0
  say "-- host kernel log since the export"
  printf '%s\n' "$out" | sed 's/^/     /'
}

run_case() { # <label> <spec> <expected model:block:type> [<expected model:block:type> ...]
  local label=$1 spec=$2
  shift 2
  local -a expected=("$@")
  say ""
  say "=== $label: oem usb-msc $spec"

  local out
  if [ "$DRY_RUN" = 1 ]; then
    note "DRY: $FASTBOOT oem usb-msc $spec"
    return 0
  fi
  local exported
  exported=$(date +%s)
  out=$("$FASTBOOT" oem usb-msc "$spec" 2>&1 || true)
  printf '%s\n' "$out" | sed 's/^/       /'
  if ! printf '%s\n' "$out" | grep -q "OKAY"; then
    skip "$label: device rejected the export"
    return 0
  fi

  local rows
  if ! rows=$(wait_for_luns "${#expected[@]}" "$LUN_TIMEOUT"); then
    fail "$label: expected ${#expected[@]} LUN(s), saw $(count_rows "$rows")"
    describe_luns "$exported"
    note "the device stays in mass-storage mode; press a volume key (or unplug)"
    return 1
  fi

  local entry model block ptype
  for entry in "${expected[@]}"; do
    model=${entry%%:*}
    block=$(printf '%s' "$entry" | cut -d: -f2)
    ptype=$(printf '%s' "$entry" | cut -d: -f3)
    local row
    row=$(lun_for_model "$rows" "$model")
    if [ -z "$row" ]; then
      fail "$label: no LUN reporting model '$model'"
      continue
    fi
    if [ "$model" = "PARTITION" ]; then
      local dev lb ro
      dev=$(row_field "$row" 1); lb=$(row_field "$row" 3); ro=$(row_field "$row" 4)
      say "-- $label: /dev/$dev (partition LUN)"
      [ "$ro" = "1" ] && pass "$label partition LUN read-only" || fail "$label partition LUN ro=$ro"
      [ "$lb" -ge 512 ] && pass "$label partition logical block size $lb" \
        || fail "$label partition logical block size $lb"
    else
      verify_file_lun "$row" "$block" "$ptype" "$label/$model"
      local first
      first=$(row_field "$row" 1)
      if write_probe "$first" "$block"; then
        pass "$label/$model write rejected, content unchanged"
      else
        fail "$label/$model write attempt changed the content"
      fi
    fi
  done

  if [ "$KEEP_MSC" = "1" ]; then
    skip "$label: left in MSC mode (--keep); eject manually"
    exit 0
  fi
  say "-- ejecting to return to Fastboot"
  local first_dev
  first_dev=$(row_field "$(printf '%s\n' "$rows" | head -n1)" 1)
  probe eject "$DEV_PREFIX$first_dev" >/dev/null 2>&1 || true
  if wait_for_no_luns 20 && wait_for_fastboot 60; then
    pass "$label returned to GBL Fastboot"
  else
    fail "$label did not return to GBL Fastboot"
    return 1
  fi
  return 0
}

# A failed command that comes back as GOOD cannot be diagnosed from SG_IO
# alone: the host log says whether usb-storage saw a short packet, a stalled
# endpooint, or ran bulk-only reset recovery.
kernel_log() { # <start-epoch>
  command -v journalctl >/dev/null 2>&1 || return 0
  local out
  out=$($SUDO journalctl -k --no-pager -o cat --since "@$1" 2>/dev/null \
        | grep -Ei "usb-storage|sd [0-9]+:[0-9]+:[0-9]+|Legal|Sense|Bulk|reset|recover|timeout|phase|short" \
        | tail -40 || true)
  [ -n "$out" ] || return 0
  say ""
  say "-- host kernel log during this run"
  printf '%s\n' "$out" | sed 's/^/   /'
}

cmd_run() {
  while [ $# -gt 0 ]; do
    case "$1" in
      --dry-run) DRY_RUN=1; shift ;;
      --keep) KEEP_MSC=1; shift ;;
      --cases) CASES=$2; shift 2 ;;
      -h|--help) usage; exit 0 ;;
      *) die "unknown run option: $1" ;;
    esac
  done

  load_state
  say "test file: $MSC_PATH (${MSC_SIZE:-?} bytes, sha256 ${MSC_SHA256:-unrecorded})"
  local started
  started=$(date +%s)

  local preexisting
  preexisting=$(count_rows "$(lun_rows)")

  if [ "$DRY_RUN" = 0 ]; then
    # An exported LUN takes the USB link away from Fastboot, so "fastboot does
    # not answer" has two very different causes. Recover from the harmless one
    # instead of asking the operator to do it.
    if [ "$preexisting" -ne 0 ]; then
      say "$preexisting GBL LUN(s) are still exported (Fastboot is unavailable while they are up); ejecting them first"
      $SUDO -v || die "sudo is required to send the eject CDB"
      eject_luns || die "could not eject the leftover LUN(s); unplug/replug USB or press Volume Up to leave mass storage, then retry"
    fi
    if ! in_gbl_fastboot; then
      note "fastboot devices: $("$FASTBOOT" devices 2>&1 | tr '\n' ' ')"
      note "adb devices:      $("$ADB" devices 2>/dev/null | tr '\n' ' ')"
      die "no GBL Fastboot interface is answering; stage the EFI and run 'oem boot-efi' (or re-enter GBL Fastboot and retry)"
    fi
    note "build identity: $(gbl_build)"
    local unlocked
    unlocked=$(fb_getvar unlocked 2 || true)
    note "unlocked: ${unlocked:-unknown}"
    $SUDO -v || die "sudo is required to read /dev/sd* read-only"
  fi

  local case
  for case in $CASES; do
    case "$case" in
      file)  run_case file "file:$MSC_PATH" "FILE IMAGE:512:0" || true ;;
      iso)   run_case iso "iso:$MSC_PATH" "ISO IMAGE:2048:5" || true ;;
      mixed) run_case mixed "boot_a,file:$MSC_PATH" "PARTITION:*:0" "FILE IMAGE:512:0" || true ;;
      *) die "unknown case: $case" ;;
    esac
  done

  if [ "$DRY_RUN" = 0 ]; then
    kernel_log "$started"
  fi

  say ""
  say "checks passed: $CHECKS, failed: $FAILURES, skipped: $SKIPS"
  [ "$FAILURES" -eq 0 ]
}

cmd_status() {
  say "state file: $STATE_FILE"
  if [ -f "$STATE_FILE" ]; then sed 's/^/  /' "$STATE_FILE"; else note "missing (run prepare)"; fi
  say "GBL LUNs visible now:"
  local rows
  rows=$(lun_rows)
  if [ "$(count_rows "$rows")" -eq 0 ]; then
    note "none"
  else
    printf '%s\n' "$rows" | sed 's/^/  /'
  fi
}

cmd_list_disks() {
  lun_rows
}

# While a LUN is exported the device has switched its USB role to mass storage,
# so the Fastboot interface is gone until START STOP UNIT (LoEj=1) brings it
# back. Ejecting the first LUN tears the whole export down; later attempts on
# the same run are expected to fail and are reported as notes.
eject_luns() {
  local rows dev model block rest
  rows=$(lun_rows)
  if [ "$(count_rows "$rows")" -eq 0 ]; then
    return 0
  fi
  if [ "$DRY_RUN" = 0 ]; then
    $SUDO -v || die "sudo is required to send the eject CDB"
  fi
  while IFS='|' read -r dev model block rest; do
    [ -n "$dev" ] || continue
    say "ejecting /dev/$dev ($model)"
    probe eject "$DEV_PREFIX$dev" || note "eject left /dev/$dev (expected: one eject tears the whole export down)"
  done <<EOF
$rows
EOF
  [ "$DRY_RUN" = 1 ] && return 0
  wait_for_fastboot 45
}

cmd_eject() {
  if [ "$(count_rows "$(lun_rows)")" -eq 0 ]; then
    note "no GBL LUN is visible, nothing to eject"
    if in_gbl_fastboot; then
      note "already in GBL Fastboot ($(gbl_build))"
    fi
    return 0
  fi
  eject_luns || die "the Fastboot interface did not come back after the eject; unplug/replug USB or press Volume Up to leave mass storage, then retry"
  say "back in GBL Fastboot: $(gbl_build)"
}

# Android only: the file media backend resolves paths against the volumes the
# *firmware* enumerates, which is not necessarily where Android mounts them.
# This lists what Android can see, so a real file can be picked for --from.
cmd_volumes() {
  command -v "$ADB" >/dev/null || die "adb not found"
  local uid mounts mount
  uid=$($ADB shell su -c id 2>/dev/null || true)
  case "$uid" in
    *uid=0*) ;;
    *) die "adb root/su is required to read the vfat volumes" ;;
  esac
  mounts=$(vfat_mounts)
  [ -n "$mounts" ] || die "no vfat volume is mounted on the device"
  for mount in $mounts; do
    say "== $mount"
    { $ADB shell su -c "ls -la '$mount'" 2>/dev/null || true; } | sed 's/^/   /'
    say ""
  done
  note "GBL is loaded from efisp, which Android does not mount; a file on these"
  note "volumes is only usable if the firmware builds a SimpleFileSystem for that"
  note "same partition. Try one with: prepare --from <path> --mount <volume>"
}

cmd_self_test() {
  "$PYTHON" "$PROBE" self-test
  bash -n "$0"
  say "self-test=ok"
}

main() {
  [ $# -ge 1 ] || { usage; exit 2; }
  local command=$1
  shift
  case "$command" in
    prepare) cmd_prepare "$@" ;;
    run) cmd_run "$@" ;;
    status) cmd_status ;;
    list-disks) cmd_list_disks ;;
    eject) cmd_eject ;;
    volumes) cmd_volumes ;;
    self-test) cmd_self_test ;;
    -h|--help|help) usage ;;
    *) usage; die "unknown command: $command" ;;
  esac
}

main "$@"
