#!/usr/bin/env bash
# 105_msc_selftest.sh — offline exercise of tests/device/msc_selftest.sh.
#
# Stubs adb, fastboot, sudo and the SG_IO probe, and fakes /sys/block and the
# block devices, so the whole prepare -> run -> verify -> eject flow executes
# without a phone. Checks that a matching export passes, that a tampered
# content hash fails, and that a wrong MODE SENSE WP bit fails. It also pins the
# read-only `prepare` contract: recording an existing ESP file must not change
# anything on the device, and only `prepare --write` may create a file.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
script=$root/tests/device/msc_selftest.sh

work=$(mktemp -d "${TMPDIR:-/tmp}/gbl-msc-selftest-XXXXXX")
trap 'chmod -R u+w "$work" 2>/dev/null; rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/sys/block" "$work/dev" "$work/state" "$work/mnt/esp" "$work/sdcard"

PYTHON3=$(command -v python3)

# A firmware file that is already on the ESP; the read-only prepare records it.
seed=$work/mnt/esp/EFI/BOOT/BOOTAA64.EFI
mkdir -p "$(dirname "$seed")"
{
  printf 'GBLMSC-SELFTEST-SEED v1\n'
  head -c $((5244114 - 32)) /dev/zero
  printf 'SEEDTAIL'
} >"$seed"
[ "$(stat -c %s "$seed")" = "5244114" ] \
  || { echo "seed file has the wrong size" >&2; exit 1; }

# Everything under the fake ESP with content hashes: prepare must not change it.
esp_snapshot() {
  ( cd "$work/mnt/esp" && find . -mindepth 1 | LC_ALL=C sort | while read -r entry; do
      if [ -f "$entry" ]; then
        printf '%s %s\n' "$entry" "$(sha256sum "$entry" | cut -d' ' -f1)"
      else
        printf '%s dir\n' "$entry"
      fi
    done )
}

# --- stubs ---------------------------------------------------------------
cat >"$work/bin/sudo" <<EOF
#!/bin/sh
[ "\$1" = "-v" ] && exit 0
exec "\$@"
EOF

cat >"$work/bin/fastboot" <<'EOF'
#!/bin/sh
SYSFS=${MSC_STUB_SYSFS:?}
DEVDIR=${MSC_STUB_DEVDIR:?}
IMG=${MSC_STUB_IMAGE:?}
case "$1" in
  getvar)
    # The real fastboot on this host prints getvar output on stderr; the script
    # must merge the streams or every probe looks like "not in Fastboot".
    case "$2" in
      gbl-chainload_build)
        [ "$(cat "${MSC_STUB_MODE:?}")" = gbl ] || exit 1
        echo "gbl-chainload_build: gbl-chainload-msc-partitions" >&2 ;;
      unlocked) echo "unlocked: yes" >&2 ;;
      *) echo "unknown var" >&2 ; exit 1 ;;
    esac ;;
  reboot) exit 0 ;;
  devices) echo "b01c2a21\tfastboot" >&2 ;;
  oem)
    [ "$2" = "usb-msc" ] || exit 1
    spec=$3
    echo "$spec" >> "${MSC_STUB_LOG:?}"
    if [ -n "${MSC_STUB_NO_LUN:-}" ]; then
      # Accept the export but never surface a block device: the case must fail
      # with diagnostics instead of silently passing.
      echo "OKAY [  0.003s]"
      echo "Finished. Total time: 0.003s"
      exit 0
    fi
    mkdir -p "$SYSFS/block"
    idx=0
    for part in $(echo "$spec" | tr ',' ' '); do
      case "$part" in
        iso:*) model="ISO IMAGE"; block=2048 ;;
        file:*) model="FILE IMAGE"; block=512 ;;
        ram:*) model="RAM DISK"; block=512 ;;
        *) model="PARTITION"; block=4096 ;;
      esac
      if [ "$model" = "ISO IMAGE" ]; then
        # A CD-ROM LUN is bound by sr_mod and shows up as /dev/sr<minor>.
        name=$(printf 'sr%d' "$idx")
      else
        name=$(printf 'sd%s' "$(echo "abcdefgh" | cut -c$((idx + 1)))")
      fi
      dir="$SYSFS/block/$name"
      mkdir -p "$dir/device" "$dir/queue"
      printf 'GBL     \n' > "$dir/device/vendor"
      printf '%-16s\n' "$model" > "$dir/device/model"
      echo "$block" > "$dir/queue/logical_block_size"
      if [ "$model" = "RAM DISK" ] && [ "${MSC_STUB_RAM_WRITABLE:-0}" = "1" ]; then
        echo 0 > "$dir/ro"
      else
        echo 1 > "$dir/ro"
      fi
      if [ "$model" = "PARTITION" ] || [ "$model" = "RAM DISK" ]; then
        head -c $((1024 * 1024)) /dev/zero > "$DEVDIR/$name"
      else
        size=$(stat -c %s "$IMG")
        padded=$(( (size + block - 1) / block * block ))
        { cat "$IMG"; head -c $((padded - size)) /dev/zero; } > "$DEVDIR/$name"
        if [ -n "${MSC_STUB_TAMPER:-}" ]; then
          # last byte of the real file, inside the final partial MiB
          printf '\xff' | dd of="$DEVDIR/$name" bs=1 seek=$((size - 1)) conv=notrunc status=none
        fi
      fi
      echo $(( $(stat -c %s "$DEVDIR/$name") / 512 )) > "$dir/size"
      if [ "$model" = "RAM DISK" ] && [ "${MSC_STUB_RAM_WRITABLE:-0}" = "1" ]; then
        chmod 644 "$DEVDIR/$name"   # a writable LUN accepts writes
      else
        chmod 444 "$DEVDIR/$name"   # a read-only block device rejects writes
      fi
      idx=$((idx + 1))
    done
    echo "gbl" > /dev/null
    echo "m" > "${MSC_STUB_MODE}"
    echo "OKAY [  0.003s]"
    echo "Finished. Total time: 0.003s" ;;
  *) exit 1 ;;
esac
EOF

cat >"$work/bin/probe-python" <<'EOF'
#!/bin/sh
REAL=${MSC_STUB_REAL_PYTHON:?}
if [ "$1" != "${MSC_STUB_PROBE:?}" ]; then
  exec "$REAL" "$@"
fi
shift
cmd=$1
dev=$2
name=$(basename "$dev")
sysfs=$MSC_STUB_SYSFS/block/$name
model=$(sed -e 's/[[:space:]]*$//' "$sysfs/device/model")
block=$(cat "$sysfs/queue/logical_block_size")
case "$cmd" in
  info)
    if [ "$model" = "ISO IMAGE" ]; then type=5; else type=0; fi
    echo "inquiry.status=ok"
    echo "inquiry.type=$type"
    echo "inquiry.rmb=1"
    echo "inquiry.product=$model"
    echo "capacity.last_lba=$(( $(cat "$sysfs/size") - 1 ))"
    echo "capacity.block_size=$block"
    echo "handle=stub" ;;
  modesense)
    if [ "$model" = "ISO IMAGE" ] && [ "${MSC_STUB_NO_CAPS:-0}" != 1 ]; then
      echo "mode6.pages=01,08,0a,1c,2a"
      echo "mode6_capabilities.status=ok"
      echo "mode6_capabilities.pages=2a"
      echo "mode6_capabilities.page_length=18"
    else
      echo "mode6.pages=01,08,0a,1c"
      echo "mode6_capabilities.status=check_condition"
      echo "mode6_capabilities.sense=5/0x24"
    fi
    echo "mode6.recovery_page_length=10"
    echo "mode6.informational_page_length=8"
    echo "mode6.status=ok"
    if [ "$model" = "RAM DISK" ] && [ "${MSC_STUB_RAM_WRITABLE:-0}" = "1" ]; then
      echo "mode6.wp=0"
    else
      echo "mode6.wp=${MSC_STUB_WP:-1}"
    fi
    echo "mode6.block_descriptor_length=0"
    echo "mode6.block_count=0"
    echo "mode6.logical_block_length=0"
    echo "mode6.hex=0080001f"
    echo "mode6_dbd0.status=ok"
    echo "mode6_dbd0.block_descriptor_length=8"
    echo "mode6_dbd0.block_count=$(cat "$sysfs/size")"
    echo "mode6_dbd0.logical_block_length=$block"
    echo "mode10.status=ok"
    echo "mode6_pc1.status=check_condition"
    echo "mode6_pc1.scsi_status=2 host_status=0 driver_status=0 resid=0 len=0 duration_ms=1 sense_len=18"
    echo "mode6_bad_page.status=check_condition"
    echo "badread.status=${MSC_STUB_BADREAD:-check_condition}"
    echo "badop.status=${MSC_STUB_BADOP:-check_condition}"
    echo "handle=stub" ;;
  mmc)
    echo "configuration.status=ok"
    echo "configuration.current_profile=0008"
    echo "configuration.entry=0008:0"
    echo "toc.status=ok"
    echo "toc.first_track=1"
    echo "toc.last_track=1"
    echo "toc.track=1:4:0"
    if [ "${MSC_STUB_NO_EVENT:-0}" != 1 ]; then
      echo "event.status=ok"
      echo "event.descriptor_length=0"
      echo "event.nea=1"
      echo "event.notification_class=0"
      echo "event.supported_classes=0x10"
    fi
    echo "handle=stub" ;;
  modeselect)
    # A current-value list is accepted; WCE=1 and an unknown page are refused.
    echo "modeselect6.status=ok"
    echo "modeselect10.status=ok"
    echo "modeselect_wce.status=check_condition"
    echo "modeselect_wce.sense=5/0x26/0x00"
    echo "modeselect_badpage.status=check_condition"
    echo "handle=stub" ;;
  write)
    echo "write.last_lba=2047"
    echo "write.block_size=$block"
    echo "write.blocks=2"
    if [ "$model" = "RAM DISK" ] && [ "${MSC_STUB_RAM_WRITABLE:-0}" = "1" ]; then
      echo "write.op6=ok"
      echo "write.op10=ok"
      echo "write.op12=ok"
      echo "write.op16=ok"
      echo "write.status=ok"
      echo "write.readback=match"
      echo "write.sync=ok"
      echo "write.persist=match"
      echo "write.restore=ok"
      echo "write.restore_sync=ok"
      echo "write.restore_verify=match"
    else
      # A read-only build refuses every WRITE opcode with DATA PROTECT and
      # changes nothing; the stub never claims a writable LUN.
      echo "write.op6=refused"
      echo "write.op10=refused"
      echo "write.op12=refused"
      echo "write.op16=refused"
      echo "write.status=refused"
      echo "write.sense=7/0x27/0x00"
    fi
    echo "handle=stub" ;;
  openrw)
    echo "openrw=refused_erofs" ;;
  eject)
    for entry in "$MSC_STUB_SYSFS"/block/sd* "$MSC_STUB_SYSFS"/block/sr*; do
      [ -d "$entry" ] || continue
      rm -rf "$entry" "$MSC_STUB_DEVDIR/$(basename "$entry")"
    done
    echo "gbl" > "$MSC_STUB_MODE"
    echo "eject.status=ok" ;;
  *) echo "stub: unknown command $cmd" >&2; exit 4 ;;
esac
EOF

cat >"$work/bin/adb" <<'EOF'
#!/bin/sh
ROOT=${MSC_STUB_ADB_ROOT:?}
map() { printf '%s' "$1" | sed -e "s#^/mnt/esp#$ROOT/mnt/esp#g" -e "s#^/sdcard#$ROOT/sdcard#g"; }
case "$1" in
  push)
    src=$2; dst=$(map "$3")
    mkdir -p "$(dirname "$dst")"
    cp "$src" "$dst" ;;
  shell)
    shift 2   # "shell" + "su"
    [ "$1" = "-c" ] && shift
    command=$*
    case "$command" in
      id) echo "uid=0(root) gid=0(root)" ;;
      mount) echo "/dev/block/by-name/esp on /mnt/esp type vfat (rw,relatime)" ;;
      sync) : ;;
      *mkdir\ -p*) eval mkdir -p "$(map "$(echo "$command" | sed -e "s/^mkdir -p //" -e "s/'//g")")" ;;
      *touch*) : >"$(map "$(echo "$command" | sed -e "s/^touch //" -e "s/'//g")")" ;;
      *stat\ -c\ %s*) stat -c %s "$(map "$(echo "$command" | sed -e "s/^stat -c %s //" -e "s/'//g")")" ;;
      *sha256sum*) sha256sum "$(map "$(echo "$command" | sed -e "s/^sha256sum //" -e "s/'//g")")" ;;
      *cp*) cmd=$(echo "$command" | sed -e "s/^cp //;s/'//g")
            set -- $cmd
            cp "$(map "$1")" "$(map "$2")" ;;
      *rm\ -f*) : ;;
      *) exit 0 ;;
    esac ;;
  devices) echo "List of devices attached" ;;
  *) exit 0 ;;
esac
EOF
chmod +x "$work/bin/sudo" "$work/bin/fastboot" "$work/bin/probe-python" "$work/bin/adb"

export MSC_STUB_SYSFS=$work/sys MSC_STUB_DEVDIR=$work/dev MSC_STUB_MODE=$work/mode \
       MSC_STUB_LOG=$work/log \
       MSC_STUB_REAL_PYTHON=$PYTHON3 MSC_STUB_PROBE=$root/tests/device/msc_probe.py \
       MSC_STUB_ADB_ROOT=$work
echo gbl >"$work/mode"

# MSC_TEST_IMAGE is the file the fastboot stub exports as the file/ISO LUN.
run_selftest() {
  env MSC_FASTBOOT_BIN="$work/bin/fastboot" MSC_ADB_BIN="$work/bin/adb" \
      MSC_SUDO_BIN="$work/bin/sudo" MSC_PYTHON_BIN="$work/bin/probe-python" \
      MSC_SYSFS_ROOT="$work/sys" MSC_DEV_PREFIX="$work/dev/" \
      MSC_STUB_IMAGE="${MSC_TEST_IMAGE:-$seed}" \
      MSC_STATE_DIR="$work/state" "$@"
}

echo "== self-test =="
MSC_PYTHON_BIN=$(command -v python3) bash "$script" self-test

echo "== prepare (read-only, default --from) =="
before=$(esp_snapshot)
run_selftest bash "$script" prepare >"$work/prepare.log"
after=$(esp_snapshot)
[ "$before" = "$after" ] || { echo "prepare modified the ESP" >&2; exit 1; }
grep -q 'read-only: recorded /mnt/esp/EFI/BOOT/BOOTAA64.EFI' "$work/prepare.log"
grep -qx 'MSC_PATH=/EFI/BOOT/BOOTAA64.EFI' "$work/state/msc-selftest.env"
sha=$(sed -n 's/^MSC_SHA256=//p' "$work/state/msc-selftest.env")
[ -n "$sha" ] && [ "$sha" = "$(sha256sum "$seed" | cut -d' ' -f1)" ]
size=$(sed -n 's/^MSC_SIZE=//p' "$work/state/msc-selftest.env")
[ "$size" = "5244114" ]

echo "== prepare must reject impossible inputs =="
run_selftest bash "$script" prepare --from /EFI/NOPE.BIN >/dev/null 2>"$work/nofile.log" && exit 1 || true
grep -q 'was not found on any vfat volume' "$work/nofile.log"
run_selftest bash "$script" prepare --size 1024 >/dev/null 2>"$work/sizeonly.log" && exit 1 || true
grep -q 'only applies to --write' "$work/sizeonly.log"
[ "$(esp_snapshot)" = "$before" ] || { echo "a rejected prepare still modified the ESP" >&2; exit 1; }

echo "== run (all cases) =="
run_selftest bash "$script" run >"$work/run.log" || { cat "$work/run.log"; exit 1; }
grep -q 'checks passed: .*failed: 0' "$work/run.log"
grep -q "PASS  file/FILE IMAGE content matches the source file" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE INQUIRY peripheral type 5" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE current profile is CD-ROM" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE one data track at LBA 0" "$work/run.log"
# A CD-ROM LUN is bound by sr_mod, so the harness must look at /dev/sr*, not
# only /dev/sd*.
grep -q -- '-- iso/ISO IMAGE: /dev/sr0 (ISO IMAGE)' "$work/run.log"
grep -q "PASS  iso/ISO IMAGE MODE SENSE pages 01,08,0a,1c,2a" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE MODE SENSE page 2a capabilities" "$work/run.log"
grep -q "PASS  file/FILE IMAGE MODE SENSE page 2a rejected on a direct-access LUN" "$work/run.log"
grep -q "PASS  ram/RAM DISK write refused (DATA PROTECT)" "$work/run.log"
grep -q "PASS  ram/RAM DISK content unchanged after a refused write" "$work/run.log"
grep -q "PASS  ram/RAM DISK MODE SENSE pages 01,08,0a,1c" "$work/run.log"
grep -q "PASS  ram/RAM DISK WRITE(10) refused" "$work/run.log"
grep -q "PASS  ram/RAM DISK WRITE(16) refused" "$work/run.log"
grep -q "PASS  file/FILE IMAGE MODE SENSE page 0x01 length field 10" "$work/run.log"
grep -q "PASS  file/FILE IMAGE MODE SENSE page 0x1c length field 8" "$work/run.log"
grep -q "PASS  file/FILE IMAGE MODE SELECT(6) current values accepted" "$work/run.log"
grep -q "PASS  file/FILE IMAGE MODE SELECT WCE=1 rejected" "$work/run.log"
grep -q "PASS  ram/RAM DISK MODE SELECT unknown page rejected" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE GET EVENT STATUS NOTIFICATION" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE no media event pending (NEA)" "$work/run.log"
grep -q "PASS  iso/ISO IMAGE media event class advertised" "$work/run.log"
grep -q "PASS  mixed/FILE IMAGE write rejected, content unchanged" "$work/run.log"
grep -q "PASS  mixed returned to GBL Fastboot" "$work/run.log"
[ "$(sed -n '1p' "$work/log")" = "file:/EFI/BOOT/BOOTAA64.EFI" ]
[ "$(sed -n '2p' "$work/log")" = "iso:/EFI/BOOT/BOOTAA64.EFI" ]
[ "$(sed -n '3p' "$work/log")" = "boot_a,file:/EFI/BOOT/BOOTAA64.EFI" ]

echo "== run (a writable RAM LUN exercises the write backend) =="
MSC_STUB_RAM_WRITABLE=1 run_selftest bash "$script" run --cases ram >"$work/ramw.log" 2>&1 || { cat "$work/ramw.log"; exit 1; }
grep -q 'checks passed: .*failed: 0' "$work/ramw.log"
grep -q "PASS  ram/RAM DISK write accepted (writable LUN)" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK 2-block round-trip" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK WRITE(6/10/12/16) all accepted" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK write visible to the next read" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK SYNCHRONIZE CACHE" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK write survives the barrier" "$work/ramw.log"
grep -q "PASS  ram/RAM DISK restore verified" "$work/ramw.log"

echo "== run (tampered content must fail) =="
MSC_STUB_TAMPER=1 run_selftest bash "$script" run --cases file >"$work/tamper.log" && exit 1 || true
grep -q "FAIL  file/FILE IMAGE content hash" "$work/tamper.log"

echo "== run (wrong WP bit must fail) =="
MSC_STUB_WP=0 run_selftest bash "$script" run --cases file >"$work/wp.log" && exit 1 || true
grep -q "FAIL  file/FILE IMAGE MODE SENSE WP bit is 0" "$work/wp.log"

echo "== run (an optical LUN without page 0x2a must fail) =="
MSC_STUB_NO_CAPS=1 run_selftest bash "$script" run --cases iso >"$work/nocaps.log" 2>&1 && exit 1 || true
grep -q "FAIL  iso/ISO IMAGE MODE SENSE pages 01,08,0a,1c (expected 01,08,0a,1c,2a)" "$work/nocaps.log"
grep -q "FAIL  iso/ISO IMAGE MODE SENSE page 2a not implemented" "$work/nocaps.log"

echo "== run (a missing media event class must fail) =="
MSC_STUB_NO_EVENT=1 run_selftest bash "$script" run --cases iso >"$work/noevent.log" 2>&1 && exit 1 || true
grep -q "FAIL  iso/ISO IMAGE GET EVENT STATUS NOTIFICATION failed" "$work/noevent.log"
grep -q "FAIL  iso/ISO IMAGE media event class missing" "$work/noevent.log"

echo "== an export without a block device fails with diagnostics =="
MSC_STUB_NO_LUN=1 MSC_LUN_TIMEOUT=2 run_selftest bash "$script" run --cases file >"$work/nolun.log" 2>&1 && exit 1 || true
grep -q 'FAIL  file: expected 1 LUN(s), saw 0' "$work/nolun.log"
grep -q -- '-- block devices present now' "$work/nolun.log"
grep -q 'press a volume key' "$work/nolun.log"

echo "== run without a prepared file must refuse =="
mkdir -p "$work/empty"
MSC_STATE_DIR="$work/empty" bash "$script" run >/dev/null 2>"$work/nostate.log" && exit 1 || true
grep -q 'missing .*msc-selftest.env' "$work/nostate.log"

echo "== prepare --write (opt-in, creates the file) =="
written=$work/mnt/esp/EFI/GBLMSC/selftest.bin
MSC_TEST_IMAGE="$written" run_selftest bash "$script" prepare --write >"$work/write.log"
grep -q -- '--write: this writes /mnt/esp/EFI/GBLMSC/selftest.bin' "$work/write.log"
grep -qx 'MSC_PATH=/EFI/GBLMSC/selftest.bin' "$work/state/msc-selftest.env"
[ -f "$written" ]
wsha=$(sed -n 's/^MSC_SHA256=//p' "$work/state/msc-selftest.env")
[ "$wsha" = "$(sha256sum "$written" | cut -d' ' -f1)" ]
MSC_TEST_IMAGE="$written" run_selftest bash "$script" run --cases file >"$work/write-run.log"
grep -q 'checks passed: .*failed: 0' "$work/write-run.log"
grep -q "PASS  file/FILE IMAGE content matches the source file" "$work/write-run.log"

echo "== a leftover export is ejected automatically, then run proceeds =="
# --keep leaves the LUN exported, which also takes the Fastboot interface down
MSC_TEST_IMAGE="$written" run_selftest bash "$script" run --cases file --keep >"$work/keep.log"
[ "$(cat "$work/mode")" = "m" ]     # fastboot is unavailable in this state
MSC_TEST_IMAGE="$written" run_selftest bash "$script" run --cases file >"$work/recover.log"
grep -q 'still exported (Fastboot is unavailable' "$work/recover.log"
grep -q 'ejecting' "$work/recover.log"
grep -q 'checks passed: .*failed: 0' "$work/recover.log"

echo "== eject on an idle device is a no-op =="
MSC_TEST_IMAGE="$written" run_selftest bash "$script" eject >"$work/idle-eject.log"
grep -q 'nothing to eject' "$work/idle-eject.log"

echo "== run with neither a LUN nor Fastboot explains the state =="
echo m >"$work/mode"
MSC_TEST_IMAGE="$written" run_selftest bash "$script" run --cases file >/dev/null 2>"$work/nofb.log" && exit 1 || true
grep -q 'no GBL Fastboot interface is answering' "$work/nofb.log"
echo gbl >"$work/mode"

echo "== the rw case refuses to run without a named partition =="
echo gbl >"$work/mode"
run_selftest bash "$script" run --cases rw >/dev/null 2>"$work/rw-noenv.log" && exit 1 || true
grep -q 'set MSC_RW_PARTITION=' "$work/rw-noenv.log"

echo "== list-disks =="
run_selftest bash "$script" run --cases file --keep >/dev/null || true
run_selftest bash "$script" list-disks | grep -qx 'sda|FILE IMAGE|512|1|10243'

echo "ok 105_msc_selftest — prepare/run flow, content and MODE SENSE assertions, tamper detection, state guard"
