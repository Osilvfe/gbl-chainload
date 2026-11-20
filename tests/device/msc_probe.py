#!/usr/bin/env python3
"""msc_probe.py - inspect a GBL USB MSC LUN through SG_IO.

Standard library only. All output is ``key=value`` lines so shell callers can
grep/compare without extra tooling.

    info <dev>        INQUIRY + READ CAPACITY(10)
    modesense <dev>   MODE SENSE(6)/(10), page 0x3f and the negative cases
    mmc <dev>         GET CONFIGURATION + READ TOC (CD-ROM LUN)
    modeselect <dev>  MODE SELECT(6)/(10): current values, WCE and bad page
    write <dev> [n]   write the last n blocks (default 1), sync and restore
    eject <dev>       PREVENT ALLOW(0) + START STOP UNIT(LoEj=1)
    openrw <dev>      report whether an O_RDWR open is refused
    self-test         offline checks of the CDB builders and parsers
"""

import ctypes
import errno
import fcntl
import os
import sys

SG_IO = 0x2285
SG_DXFER_NONE = -1
SG_DXFER_TO_DEV = -2
SG_DXFER_FROM_DEV = -3

SENSE_VALUES = {
    0x11: "unrecovered read error",
    0x20: "invalid command operation code",
    0x21: "logical block address out of range",
    0x24: "invalid field in CDB",
    0x25: "logical unit not supported",
    0x27: "write protected",
    0x28: "not ready to ready change",
    0x3A: "medium not present",
    0x53: "media removal prevented",
}


class SgIoHdr(ctypes.Structure):
    _fields_ = [
        ("interface_id", ctypes.c_int),
        ("dxfer_direction", ctypes.c_int),
        ("cmd_len", ctypes.c_ubyte),
        ("mx_sb_len", ctypes.c_ubyte),
        ("iovec_count", ctypes.c_ushort),
        ("dxfer_len", ctypes.c_uint),
        ("dxferp", ctypes.c_void_p),
        ("cmdp", ctypes.c_void_p),
        ("sbp", ctypes.c_void_p),
        ("timeout", ctypes.c_uint),
        ("flags", ctypes.c_uint),
        ("pack_id", ctypes.c_int),
        ("usr_ptr", ctypes.c_void_p),
        ("status", ctypes.c_ubyte),
        ("masked_status", ctypes.c_ubyte),
        ("msg_status", ctypes.c_ubyte),
        ("sb_len_wr", ctypes.c_ubyte),
        ("host_status", ctypes.c_ushort),
        ("driver_status", ctypes.c_ushort),
        ("resid", ctypes.c_int),
        ("duration", ctypes.c_uint),
        ("info", ctypes.c_uint),
    ]


class ProbeError(RuntimeError):
    pass


def cdb_inquiry(alloc=36, evpd=0, page=0):
    return bytes([0x12, evpd, page, 0, alloc, 0])


def cdb_mode_sense6(page, pc=0, dbd=1, alloc=0x3F):
    return bytes([0x1A, 0x08 if dbd else 0x00, (pc << 6) | (page & 0x3F), 0, alloc, 0])


def cdb_mode_sense10(page, pc=0, dbd=1, alloc=0x3C):
    return bytes([0x5A, 0x08 if dbd else 0x00, (pc << 6) | (page & 0x3F),
                  0, 0, 0, 0, (alloc >> 8) & 0xFF, alloc & 0xFF, 0])


def cdb_read_capacity10():
    return bytes([0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0])


def cdb_get_configuration(rt=0, feature=0, alloc=0xFF):
    return bytes([0x46, rt & 3, (feature >> 8) & 0xFF, feature & 0xFF,
                  0, 0, 0, (alloc >> 8) & 0xFF, alloc & 0xFF, 0])


def cdb_read_toc(msf=0, fmt=0, alloc=0x100):
    return bytes([0x43, 0x02 if msf else 0x00, fmt & 0x0F, 0, 0, 0, 0,
                  (alloc >> 8) & 0xFF, alloc & 0xFF, 0])


def cdb_read10(lba=0, blocks=1):
    return bytes([0x28, 0, (lba >> 24) & 0xFF, (lba >> 16) & 0xFF,
                  (lba >> 8) & 0xFF, lba & 0xFF, 0, (blocks >> 8) & 0xFF, blocks & 0xFF, 0])


def cdb_write10(lba=0, blocks=1):
    return bytes([0x2A, 0, (lba >> 24) & 0xFF, (lba >> 16) & 0xFF,
                  (lba >> 8) & 0xFF, lba & 0xFF, 0, (blocks >> 8) & 0xFF, blocks & 0xFF, 0])


def cdb_sync_cache10():
    return bytes([0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0])
def cdb_write6(lba=0, blocks=1):
    if not 1 <= blocks <= 256:
        raise ValueError("WRITE(6) supports 1..256 blocks")
    lba &= 0x1FFFFF
    n = 0 if blocks == 256 else blocks
    return bytes([0x0A, (lba >> 16) & 0x1F, (lba >> 8) & 0xFF, lba & 0xFF, n, 0])


def cdb_write12(lba=0, blocks=1):
    return bytes([0xAA, 0, (lba >> 24) & 0xFF, (lba >> 16) & 0xFF,
                  (lba >> 8) & 0xFF, lba & 0xFF, 0,
                  (blocks >> 24) & 0xFF, (blocks >> 16) & 0xFF,
                  (blocks >> 8) & 0xFF, blocks & 0xFF, 0])


def cdb_write16(lba=0, blocks=1):
    return bytes([0x8A, 0,
                  (lba >> 56) & 0xFF, (lba >> 48) & 0xFF, (lba >> 40) & 0xFF,
                  (lba >> 32) & 0xFF, (lba >> 24) & 0xFF, (lba >> 16) & 0xFF,
                  (lba >> 8) & 0xFF, lba & 0xFF,
                  (blocks >> 24) & 0xFF, (blocks >> 16) & 0xFF,
                  (blocks >> 8) & 0xFF, blocks & 0xFF, 0, 0])


def cdb_mode_select6(parameter_length):
    return bytes([0x15, 0x00, 0, 0, parameter_length & 0xFF, 0])


def cdb_mode_select10(parameter_length):
    return bytes([0x55, 0, 0, 0, 0, 0, 0,
                  (parameter_length >> 8) & 0xFF, parameter_length & 0xFF, 0])


def mode_select_list(page, ten=False):
    """Build a well-formed MODE SELECT parameter list holding one page.

    Page 0x08 with a zero first body byte asks for the write cache already in
    effect (WCE=0); the block descriptor length is zero because the host does
    not re-declare the geometry. ``page`` is the full page bytes (code, length,
    body)."""
    if ten:
        header = bytearray(8)
        total = 8 + len(page)
        header[0] = (total - 2) >> 8
        header[1] = (total - 2) & 0xFF
    else:
        header = bytearray(4)
        header[0] = (4 + len(page) - 1) & 0xFF
    return bytes(header) + bytes(page)


def caching_page(wce=0, length=18):
    body = bytearray(2 + length)
    body[0] = 0x08
    body[1] = length
    if wce:
        body[2] |= 0x04
    return bytes(body)



def cdb_get_event_status_notification(rt=1, alloc=0x10):
    return bytes([0x4A, rt & 1, 0, 0, 0, 0, 0, (alloc >> 8) & 0xFF, alloc & 0xFF, 0])


def cdb_prevent_allow(prevent=0):
    return bytes([0x1E, 0, 0, 0, 1 if prevent else 0, 0])


def cdb_start_stop(start=0, loej=1):
    return bytes([0x1B, 0, 0, 0, (0x02 if loej else 0) | (0x01 if start else 0), 0])


def parse_inquiry(data):
    if len(data) < 5:
        raise ProbeError("short INQUIRY response")
    return {
        "type": data[0] & 0x1F,
        "rmb": (data[1] >> 7) & 1,
        "version": data[2],
        "response_format": data[3] & 0x0F,
        "additional_length": data[4],
        "vendor": data[8:16].decode("ascii", "replace").strip(),
        "product": data[16:32].decode("ascii", "replace").strip(),
        "revision": data[32:36].decode("ascii", "replace").strip() if len(data) >= 36 else "",
    }


def parse_capacity(data):
    if len(data) < 8:
        raise ProbeError("short READ CAPACITY response")
    return {
        "last_lba": int.from_bytes(data[0:4], "big"),
        "block_size": int.from_bytes(data[4:8], "big"),
    }


def parse_mode_sense(data, ten):
    header = 8 if ten else 4
    if len(data) < header:
        raise ProbeError("short MODE SENSE response")
    if ten:
        length = int.from_bytes(data[0:2], "big")
        block_length = int.from_bytes(data[6:8], "big")
        total = length + 2
    else:
        length = data[0]
        block_length = data[3]
        total = length + 1
    # Trust the declared length: a device may answer with the whole allocation
    # length, padded with zeros, and those zeros would otherwise parse as a
    # stream of empty page 0x00 entries.
    if total > len(data):
        raise ProbeError("mode data length %d exceeds %d bytes" % (total, len(data)))
    if total < header:
        raise ProbeError("mode data length %d is below the %d-byte header" % (total, header))
    if total < len(data):
        data = data[:total]
    result = {
        "mode_data_length": length,
        "wp": (data[3 if ten else 2] >> 7) & 1,
        "block_descriptor_length": block_length,
        "block_count": 0,
        "logical_block_length": 0,
        "pages": "",
    }
    offset = header
    if block_length >= 8:
        result["block_count"] = int.from_bytes(data[offset + 1:offset + 4], "big")
        result["logical_block_length"] = int.from_bytes(data[offset + 5:offset + 8], "big")
        offset += block_length
    codes = []
    while offset + 2 <= len(data):
        page = data[offset] & 0x3F
        size = data[offset + 1]
        body = data[offset + 2:offset + 2 + size]
        codes.append("%02x" % page)
        if page == 0x08:
            result["caching_rcd"] = body[0] & 1 if len(body) > 0 else -1
            result["caching_wce"] = (body[0] >> 2) & 1 if len(body) > 0 else -1
        if page == 0x0A:
            result["control_gltsd"] = (body[0] >> 2) & 1 if len(body) > 0 else -1
            result["control_dsense"] = (body[0] >> 3) & 1 if len(body) > 0 else -1
        if page == 0x01:
            result["recovery_page_length"] = size
        if page == 0x1C:
            result["informational_page_length"] = size
        if page == 0x2A:
            result["capabilities_page_length"] = size
        offset += 2 + size
    result["pages"] = ",".join(codes)
    return result


def parse_configuration(data):
    if len(data) < 8:
        raise ProbeError("short GET CONFIGURATION response")
    return {
        "data_length": int.from_bytes(data[0:4], "big"),
        "current_profile": data[6:8].hex(),
        "profiles": [],
        "features": [],
    }


def parse_toc(data):
    if len(data) < 4:
        raise ProbeError("short READ TOC response")
    result = {
        "data_length": int.from_bytes(data[0:2], "big"),
        "first_track": data[2],
        "last_track": data[3],
        "tracks": [],
    }
    offset = 4
    while offset + 8 <= len(data) and offset + 8 <= 4 + result["data_length"]:
        result["tracks"].append({
            "number": data[offset + 2],
            "control": data[offset + 1] & 0x0F,
            "adr": (data[offset + 1] >> 4) & 0x0F,
            "address": int.from_bytes(data[offset + 4:offset + 8], "big"),
        })
        offset += 8
    return result


def _scsi_generic_node(block_dev):
    name = os.path.basename(block_dev)
    base = "/sys/block/%s/device/scsi_generic" % name
    try:
        for entry in sorted(os.listdir(base)):
            node = "/dev/" + entry
            if os.path.exists(node):
                return node
    except OSError:
        pass
    return None


def _bsg_node(block_dev):
    name = os.path.basename(block_dev)
    try:
        target = os.path.realpath("/sys/block/%s" % name)
    except OSError:
        return None
    hctl = os.path.basename(os.path.dirname(target))
    node = "/dev/bsg/" + hctl
    return node if os.path.exists(node) else None


def open_scsi(dev):
    """Return (fd, description). Try /dev/sg*, /dev/bsg/*, then the block node."""
    candidates = []
    name = os.path.basename(dev)
    path = dev if dev.startswith("/dev/") else "/dev/" + name
    sg = _scsi_generic_node(path)
    if sg:
        candidates.append(sg)
    bsg = _bsg_node(path)
    if bsg:
        candidates.append(bsg)
    candidates.append(path)
    errors = []
    for candidate in candidates:
        try:
            fd = os.open(candidate, os.O_RDWR)
        except OSError as exc:
            try:
                fd = os.open(candidate, os.O_RDONLY)
            except OSError as exc2:
                errors.append("%s: %s" % (candidate, exc2.strerror or exc2))
                continue
        return fd, candidate
    raise ProbeError("no SG_IO capable handle (%s)" % "; ".join(errors or ["none"]))


LAST_HOST = 0
LAST_DRIVER = 0
LAST_RESID = 0
LAST_SCSI_STATUS = 0
LAST_DURATION = 0
LAST_SB_LEN = 0
LAST_DATA = b""


def sg_io(fd, cdb, data_len=0, direction=SG_DXFER_NONE, timeout_ms=20000, payload=None):
    global LAST_HOST, LAST_DRIVER, LAST_RESID, LAST_SCSI_STATUS, LAST_DURATION
    global LAST_SB_LEN, LAST_DATA
    if payload is not None:
        # A data-out command: the buffer carries the payload the device reads.
        data_len = len(payload)
    data = ctypes.create_string_buffer(max(data_len, 1))
    if payload is not None and data_len:
        ctypes.memmove(data, bytes(payload), data_len)
    sense = ctypes.create_string_buffer(96)
    cmd = ctypes.create_string_buffer(bytes(cdb), len(cdb))
    hdr = SgIoHdr()
    hdr.interface_id = ord("S")
    hdr.dxfer_direction = direction
    hdr.cmd_len = len(cdb)
    hdr.mx_sb_len = ctypes.sizeof(sense)
    hdr.dxfer_len = data_len
    hdr.dxferp = ctypes.cast(data, ctypes.c_void_p)
    hdr.cmdp = ctypes.cast(cmd, ctypes.c_void_p)
    hdr.sbp = ctypes.cast(sense, ctypes.c_void_p)
    hdr.timeout = timeout_ms
    # Hand the kernel a writable copy of the header and read it back afterwards.
    # Passing ``bytes (hdr)`` would leave the original structure untouched, so
    # status, host_status, resid and sb_len_wr would all read as zero and a
    # CHECK CONDITION would look like GOOD.
    hdr_buf = ctypes.create_string_buffer (bytes (hdr), ctypes.sizeof (SgIoHdr))
    try:
        fcntl.ioctl (fd, SG_IO, hdr_buf, True)
    except OSError as exc:
        raise ProbeError ("SG_IO failed: %s" % exc.strerror)
    hdr = SgIoHdr.from_buffer (hdr_buf)
    status = hdr.status
    LAST_HOST = hdr.host_status
    LAST_DRIVER = hdr.driver_status
    LAST_RESID = hdr.resid
    LAST_SCSI_STATUS = status
    LAST_DURATION = hdr.duration
    LAST_SB_LEN = hdr.sb_len_wr
    sb = sense.raw[:min (hdr.sb_len_wr, ctypes.sizeof (sense))]
    payload = data.raw[:max (data_len - hdr.resid, 0)] if data_len else b""
    # Whatever the kernel copied into the buffer, independent of the reported
    # residue: a device that reports residue for data it did transfer would
    # otherwise look like it sent nothing.
    LAST_DATA = data.raw if data_len else b""
    return status, sb, payload, hdr.resid


def sense_summary(sb):
    if sb is None or len(sb) < 14 or (sb[0] & 0x7F) not in (0x70, 0x71):
        return None
    return sb[2] & 0x0F, sb[12], sb[13]


def transport_facts(tag, raw):
    """Print the SG_IO result facts that separate the failure modes.

    A command that failed before its data stage can reach the host as: real
    sense data (scsi_status 2), a transport error (host_status != 0), a data
    stage filled with the CSW itself (hex starts with the 'USBS' signature), or
    GOOD with whatever stale/zero bytes the buffer held.
    """
    fields = {
        "scsi_status": LAST_SCSI_STATUS,
        "host_status": LAST_HOST,
        "driver_status": LAST_DRIVER,
        "resid": LAST_RESID,
        "len": len(raw),
        "duration_ms": LAST_DURATION,
        "sense_len": LAST_SB_LEN,
        "csw_in_data": 1 if raw[:4] == b"USBS" else 0,
    }
    print("%s.%s" % (tag, " ".join("%s=%s" % (k, v) for k, v in fields.items())))
    print("%s.hex=%s" % (tag, raw[:32].hex()))
    # Buffer contents as the kernel left them, not truncated by the reported
    # residue.
    print("%s.buf=%s" % (tag, LAST_DATA[:32].hex()))


def emit(mapping):
    for key in sorted(mapping):
        print("%s=%s" % (key, mapping[key]))


def cmd_info(dev):
    fd, how = open_scsi(dev)
    try:
        status, sb, data, _ = sg_io(fd, cdb_inquiry(), 36, SG_DXFER_FROM_DEV)
        info = parse_inquiry(data) if status == 0 else {}
        if status != 0:
            key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
            print("inquiry.status=check_condition")
            print("inquiry.sense=%d/0x%02x/0x%02x" % (key, asc & 0xFF, ascq & 0xFF))
        else:
            print("inquiry.status=ok")
            emit({"inquiry." + k: v for k, v in info.items()})
        status, sb, data, _ = sg_io(fd, cdb_read_capacity10(), 8, SG_DXFER_FROM_DEV)
        if status == 0:
            emit({"capacity." + k: v for k, v in parse_capacity(data).items()})
        else:
            key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
            print("capacity.status=check_condition")
            print("capacity.sense=%d/0x%02x/0x%02x" % (key, asc & 0xFF, ascq & 0xFF))
        print("handle=%s" % how)
    finally:
        os.close(fd)


def _mode_sense(fd, ten, page, pc=0, dbd=1):
    # Ask for the whole allocation. MODE SENSE(6) can return up to 255 bytes and
    # MODE SENSE(10) more; the full page set (header + block descriptor + pages
    # 0x01/0x08/0x0a/0x1c, plus 0x2a on an optical LUN) is 64 and 80 bytes, so a
    # 63/60-byte request would truncate it and parse as a short read.
    if ten:
        status, sb, data, _ = sg_io(fd, cdb_mode_sense10(page, pc, dbd, 0x0100),
                                    0x0100, SG_DXFER_FROM_DEV)
    else:
        status, sb, data, _ = sg_io(fd, cdb_mode_sense6(page, pc, dbd, 0xFF),
                                    0xFF, SG_DXFER_FROM_DEV)
    if status != 0:
        return None, sb, b""
    try:
        return parse_mode_sense(data, ten), sb, data
    except ProbeError as exc:
        return "invalid:%s" % exc, sb, data


def cmd_modesense(dev):
    fd, how = open_scsi(dev)
    failures = 0
    try:
        for ten in (False, True):
            tag = "mode10" if ten else "mode6"
            info, sb, raw = _mode_sense(fd, ten, 0x3F)
            if info is None:
                key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
                print("%s.status=check_condition" % tag)
                print("%s.sense=%d/0x%02x" % (tag, key, asc & 0xFF))
                failures += 1
                continue
            if isinstance(info, str):
                print("%s.status=%s" % (tag, info))
                failures += 1
                continue
            info["status"] = "ok"
            info["hex"] = raw[:48].hex()
            info["resid"] = LAST_RESID
            info["len"] = len(raw)
            emit({tag + "." + k: v for k, v in info.items()})
        # DBD=0 must produce the 8-byte block descriptor the host relies on.
        info, _, _ = _mode_sense(fd, False, 0x3F, 0, 0)
        if info is None:
            print("mode6_dbd0.status=check_condition")
            failures += 1
        elif isinstance(info, str):
            print("mode6_dbd0.status=%s" % info)
            failures += 1
        else:
            info["status"] = "ok"
            emit({"mode6_dbd0." + k: v for k, v in info.items()
                  if k in ("status", "block_descriptor_length", "block_count",
                           "logical_block_length")})
        # Page 0x00 and PC=2 must work; changeable values (PC=1) and an unknown
        # page must be a defined ILLEGAL REQUEST. A failed data-in command has
        # to come back as CHECK CONDITION, never as GOOD with a zeroed buffer:
        # report the transport facts for every case so a silent zero-fill can
        # be told apart from a real short read.
        for tag, ten, page, pc, expect_ok in (
                ("mode6_page0", False, 0x00, 0, True),
                ("mode6_pc2", False, 0x08, 2, True),
                ("mode6_pc1", False, 0x08, 1, False),
                ("mode6_bad_page", False, 0x02, 0, False),
                ("mode10_bad_page", True, 0x02, 0, False)):
            info, sb, raw = _mode_sense(fd, ten, page, pc, 1)
            key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
            if info is None:
                print("%s.status=check_condition" % tag)
                print("%s.sense=%d/0x%02x/0x%02x" % (tag, key, asc & 0xFF, ascq & 0xFF))
                if expect_ok:
                    failures += 1
            elif isinstance(info, str):
                print("%s.status=%s" % (tag, info))
                failures += 1
            else:
                print("%s.status=ok" % tag)
                if not expect_ok:
                    failures += 1
            transport_facts(tag, raw)
        # MMC page 0x2A (CD/DVD capabilities) only exists for optical LUNs.
        # Ask with DBD=1 so the reply is the header plus the page alone; a
        # direct-access LUN must refuse it like any other unknown page.
        info, sb, raw = _mode_sense(fd, False, 0x2A, 0, 1)
        key, asc, _ = sense_summary(sb) or (-1, -1, -1)
        if info is None:
            print("mode6_capabilities.status=check_condition")
            print("mode6_capabilities.sense=%d/0x%02x" % (key, asc & 0xFF))
        elif isinstance(info, str):
            print("mode6_capabilities.status=%s" % info)
        else:
            print("mode6_capabilities.status=ok")
            print("mode6_capabilities.pages=%s" % info["pages"])
            print("mode6_capabilities.page_length=%s"
                  % info.get("capabilities_page_length", "missing"))
        # The same failed-data-in machinery serves a READ past the end of the
        # medium; hosts must see CHECK CONDITION there too, not zeros.
        status, sb, raw, _ = sg_io(fd, bytes([0x28, 0, 0x7F, 0xFF, 0xFF, 0xFF, 0, 0, 1, 0]),
                                   0x200, SG_DXFER_FROM_DEV)
        key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
        print("badread.status=%s" % ("ok" if status == 0 else "check_condition"))
        print("badread.sense=%d/0x%02x/0x%02x" % (key, asc & 0xFF, ascq & 0xFF))
        transport_facts("badread", raw)
        if status == 0:
            failures += 1
        # Control: a command rejected with no data stage must also come back
        # as CHECK CONDITION. If this one passes while the data-in cases fail,
        # the CSW status path is fine and only the failed data stage is wrong.
        status, sb, raw, _ = sg_io(fd, bytes([0xFF, 0, 0, 0, 0, 0]), 0, SG_DXFER_NONE)
        key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
        print("badop.status=%s" % ("ok" if status == 0 else "check_condition"))
        print("badop.sense=%d/0x%02x/0x%02x" % (key, asc & 0xFF, ascq & 0xFF))
        transport_facts("badop", raw)
        if status == 0:
            failures += 1
        print("handle=%s" % how)
    finally:
        os.close(fd)
    return 1 if failures else 0


def cmd_mmc(dev):
    fd, how = open_scsi(dev)
    failures = 0
    try:
        status, sb, data, _ = sg_io(fd, cdb_get_configuration(0, 0, 0x30), 0x30, SG_DXFER_FROM_DEV)
        if status == 0:
            config = parse_configuration(data)
            print("configuration.status=ok")
            print("configuration.current_profile=%s" % config["current_profile"])
            for offset in range(8, min(4 + config["data_length"], len(data)), 4):
                entry = data[offset:offset + 4]
                if len(entry) < 4:
                    break
                print("configuration.entry=%s:%s" % (entry[0:2].hex(), int.from_bytes(entry[2:4], "big")))
        else:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("configuration.status=check_condition")
            print("configuration.sense=%d/0x%02x" % (key, asc & 0xFF))
            failures += 1
        status, sb, data, _ = sg_io(fd, cdb_read_toc(0, 0, 0x40), 0x40, SG_DXFER_FROM_DEV)
        if status == 0:
            toc = parse_toc(data)
            print("toc.status=ok")
            print("toc.first_track=%d" % toc["first_track"])
            print("toc.last_track=%d" % toc["last_track"])
            for track in toc["tracks"]:
                print("toc.track=%d:%d:%d" % (track["number"], track["control"], track["address"]))
        else:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("toc.status=check_condition")
            print("toc.sense=%d/0x%02x" % (key, asc & 0xFF))
            failures += 1
        # GET EVENT STATUS NOTIFICATION: the data length counts the event
        # descriptor only, NEA says no event is pending, and the supported
        # class mask advertises the media class (bit 4).
        status, sb, data, _ = sg_io(fd, cdb_get_event_status_notification(1, 0x10),
                                    0x10, SG_DXFER_FROM_DEV)
        if status == 0 and len(data) >= 6:
            print("event.status=ok")
            print("event.descriptor_length=%d" % int.from_bytes(data[0:2], "big"))
            print("event.nea=%d" % ((data[2] >> 7) & 1))
            print("event.notification_class=%d" % (data[2] & 7))
            print("event.supported_classes=0x%02x" % data[5])
        else:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("event.status=check_condition")
            print("event.sense=%d/0x%02x" % (key, asc & 0xFF))
            failures += 1
        print("handle=%s" % how)
    finally:
        os.close(fd)
    return 1 if failures else 0


def cmd_modeselect(dev):
    """MODE SELECT(6)/(10) as the device enforces it.

    A parameter list that re-states the values already in effect (page 0x08
    with WCE=0) is accepted and changes nothing. Asking for a write cache
    (WCE=1) or naming an unknown page is INVALID FIELD IN PARAMETER LIST: the
    device never accepts a policy it cannot honour.
    """
    fd, how = open_scsi(dev)
    failures = 0
    try:
        current6 = mode_select_list(caching_page(0), ten=False)
        current10 = mode_select_list(caching_page(0), ten=True)
        wce = mode_select_list(caching_page(1), ten=False)
        unknown = mode_select_list(bytes([0x02, 0x02, 0x00, 0x00]), ten=False)
        cases = (
            ("modeselect6", cdb_mode_select6(len(current6)), current6, True),
            ("modeselect10", cdb_mode_select10(len(current10)), current10, True),
            ("modeselect_wce", cdb_mode_select6(len(wce)), wce, False),
            ("modeselect_badpage", cdb_mode_select6(len(unknown)), unknown, False),
        )
        for tag, cdb, lst, expect_ok in cases:
            status, sb, _, _ = sg_io(fd, cdb, len(lst), SG_DXFER_TO_DEV, payload=lst)
            key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
            if status == 0:
                print("%s.status=ok" % tag)
                if not expect_ok:
                    failures += 1
            else:
                print("%s.status=check_condition" % tag)
                print("%s.sense=%d/0x%02x/0x%02x" % (tag, key, asc & 0xFF, ascq & 0xFF))
                if expect_ok:
                    failures += 1
        print("handle=%s" % how)
    finally:
        os.close(fd)
    return 1 if failures else 0


def _pattern(size, seed):
    return bytes(((i * 37 + seed) & 0xFF) for i in range(size))


def cmd_write(dev, rest=None):
    """Exercise the write path, then put the original bytes back.

    The target range ends at the last block of the medium, so a writable LUN's
    filesystem metadata is untouched and the original content is restored
    before the command returns. The probe covers, in order:

      1. each WRITE(6/10/12/16) opcode on the last block, so a writable LUN is
         shown to accept every CDB size and a read-only LUN to refuse them all
         with DATA PROTECT;
      2. a multi-block pattern written with WRITE(10), read straight back;
      3. SYNCHRONIZE CACHE and a re-read (the bytes survive the barrier);
      4. the original bytes written back, flushed, and verified.

    A read-only LUN answers DATA PROTECT and nothing is written: that is
    reported as ``write.status=refused`` with the sense data, not as a failure
    of the probe itself. ``write <dev> [blocks]`` widens the round-trip to up
    to 8 blocks (default 1).
    """
    rest = rest or []
    try:
        want_blocks = int(rest[0]) if rest else 1
    except ValueError:
        print("write.status=error")
        print("error=blocks must be an integer", file=sys.stderr)
        return 1
    want_blocks = max(1, want_blocks)
    fd, how = open_scsi(dev)
    failures = 0
    try:
        status, sb, data, _ = sg_io(fd, cdb_read_capacity10(), 8, SG_DXFER_FROM_DEV)
        if status != 0 or len(data) < 8:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("write.status=check_condition")
            print("write.sense=%d/0x%02x" % (key, asc & 0xFF))
            return 1
        last_lba = int.from_bytes(data[0:4], "big")
        block = int.from_bytes(data[4:8], "big")
        blocks = min(want_blocks, last_lba + 1, 8)
        first_lba = last_lba - blocks + 1
        print("write.last_lba=%d" % last_lba)
        print("write.block_size=%d" % block)
        print("write.blocks=%d" % blocks)
        status, sb, original, _ = sg_io(fd, cdb_read10(first_lba, blocks), block * blocks,
                                        SG_DXFER_FROM_DEV)
        if status != 0 or len(original) != block * blocks:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("write.status=check_condition")
            print("write.sense=%d/0x%02x" % (key, asc & 0xFF))
            return 1

        # 1) Every WRITE opcode size on the single last block.
        opcodes = ((6, cdb_write6), (10, cdb_write10), (12, cdb_write12), (16, cdb_write16))
        refused = 0
        tested = 0
        first_refused = None
        for size, cdb in opcodes:
            tag = "write.op%d" % size
            if size == 6 and last_lba > 0x1FFFFF:
                print("%s=skip" % tag)
                continue
            tested += 1
            pat = _pattern(block, size)
            status, sb, _, _ = sg_io(fd, cdb(last_lba, 1), block, SG_DXFER_TO_DEV, payload=pat)
            if status != 0:
                key, asc, ascq = sense_summary(sb) or (-1, -1, -1)
                print("%s=refused" % tag)
                print("%s_sense=%d/0x%02x/0x%02x" % (tag, key, asc & 0xFF, ascq & 0xFF))
                refused += 1
                if first_refused is None:
                    first_refused = (key, asc, ascq)
                continue
            status2, sb2, back, _ = sg_io(fd, cdb_read10(last_lba, 1), block, SG_DXFER_FROM_DEV)
            ok = status2 == 0 and back == pat
            print("%s=%s" % (tag, "ok" if ok else "mismatch"))
            if not ok:
                failures += 1

        if tested != 0 and refused == tested:
            # Read-only LUN: every write was refused, the medium is intact.
            key, asc, ascq = first_refused or (-1, -1, -1)
            print("write.status=refused")
            print("write.sense=%d/0x%02x/0x%02x" % (key, asc & 0xFF, ascq & 0xFF))
            transport_facts("write", b"")
            print("handle=%s" % how)
            return 1 if failures else 0
        if refused:
            # Some opcodes accepted, others refused: a consistency bug.
            print("write.status=partial")
            failures += 1
            return 1

        # 2) Multi-block pattern round-trip with WRITE(10).
        size = block * blocks
        pat = _pattern(size, 1)
        status, sb, _, _ = sg_io(fd, cdb_write10(first_lba, blocks), size, SG_DXFER_TO_DEV,
                                 payload=pat)
        print("write.status=%s" % ("ok" if status == 0 else "check_condition"))
        if status != 0:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("write.sense=%d/0x%02x" % (key, asc & 0xFF))
            return 1

        def readback() -> bytes:
            st, sbx, out, _ = sg_io(fd, cdb_read10(first_lba, blocks), size, SG_DXFER_FROM_DEV)
            return out if st == 0 else b""

        back = readback()
        print("write.readback=%s" % ("match" if back == pat else "mismatch"))
        if back != pat:
            failures += 1

        # 3) The barrier, then confirm the bytes are still there.
        status, sb, _, _ = sg_io(fd, cdb_sync_cache10(), 0, SG_DXFER_NONE)
        print("write.sync=%s" % ("ok" if status == 0 else "check_condition"))
        if status != 0:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("write.sync_sense=%d/0x%02x" % (key, asc & 0xFF))
            failures += 1
        back = readback()
        print("write.persist=%s" % ("match" if back == pat else "mismatch"))
        if back != pat:
            failures += 1

        # 4) Restore the original bytes and verify the restore stuck.
        status, sb, _, _ = sg_io(fd, cdb_write10(first_lba, blocks), size, SG_DXFER_TO_DEV,
                                 payload=original)
        print("write.restore=%s" % ("ok" if status == 0 else "check_condition"))
        if status != 0:
            failures += 1
        status, sb, _, _ = sg_io(fd, cdb_sync_cache10(), 0, SG_DXFER_NONE)
        print("write.restore_sync=%s" % ("ok" if status == 0 else "check_condition"))
        if status != 0:
            failures += 1
        back = readback()
        print("write.restore_verify=%s" % ("match" if back == original else "mismatch"))
        if back != original:
            failures += 1
        print("handle=%s" % how)
    finally:
        os.close(fd)
    return 1 if failures else 0


def cmd_eject(dev):
    fd, how = open_scsi(dev)
    try:
        sg_io(fd, cdb_prevent_allow(0), 0, SG_DXFER_NONE)
        status, sb, _, _ = sg_io(fd, cdb_start_stop(0, 1), 0, SG_DXFER_NONE)
        if status != 0:
            key, asc, _ = sense_summary(sb) or (-1, -1, -1)
            print("eject.status=check_condition")
            print("eject.sense=%d/0x%02x" % (key, asc & 0xFF))
            return 1
        print("eject.status=ok")
        print("handle=%s" % how)
        return 0
    finally:
        os.close(fd)


def cmd_openrw(dev):
    path = dev if dev.startswith("/dev/") else "/dev/" + dev
    try:
        fd = os.open(path, os.O_RDWR)
    except OSError as exc:
        if exc.errno == errno.EROFS:
            print("openrw=refused_erofs")
        elif exc.errno == errno.EACCES:
            print("openrw=refused_eacces")
        else:
            print("openrw=refused_errno_%d" % exc.errno)
        return 0
    os.close(fd)
    print("openrw=ok")
    return 1


def self_test_sgio_header():
    """Pin how SG_IO results come back from the kernel.

    The kernel fills the header in place, so the probe has to hand over a
    writable buffer and read the result back out of it. Handing over an
    immutable copy leaves every field at zero, which silently turns a CHECK
    CONDITION into "GOOD with a zeroed buffer" - the exact misreading that made
    three validation runs useless.
    """
    saved = fcntl.ioctl

    def fake_ioctl(fd, request, arg, mutate=True):
        assert request == SG_IO and mutate is True, (request, mutate)
        live = SgIoHdr.from_buffer(arg)          # fails unless arg is writable
        live.status = 2
        live.host_status = 0
        live.driver_status = 0
        live.resid = 5
        live.duration = 12
        live.sb_len_wr = 18
        ctypes.memmove(live.sbp, bytes([0x70, 0, 5, 0, 0, 0, 0, 10,
                                        0, 0, 0, 0, 0x24, 0x21, 0, 0, 0, 0]), 18)
        ctypes.memmove(live.dxferp, b"ABC", 3)
        return 0

    fcntl.ioctl = fake_ioctl
    try:
        status, sb, payload, resid = sg_io(1, cdb_mode_sense6(0x08, 1, 1), 8,
                                           SG_DXFER_FROM_DEV)
    finally:
        fcntl.ioctl = saved
    assert status == 2, status
    assert resid == 5 and LAST_RESID == 5, (resid, LAST_RESID)
    assert (LAST_HOST, LAST_DRIVER, LAST_DURATION) == (0, 0, 12), LAST_DURATION
    assert sense_summary(sb) == (5, 0x24, 0x21), sb[:14].hex()
    assert payload == b"ABC", payload


def _fake_mode_body(dbd, ten=False, capabilities=False):
    """MODE SENSE reply shaped like the firmware's: pages 0x08, 0x0a and 0x2a."""
    header = 8 if ten else 4
    descriptor = 0 if dbd else 8
    caching = bytes([0x08, 18, 0x00] + [0] * 17)
    control = bytes([0x0A, 8, 0x04] + [0] * 7)
    caps = bytes([0x2A, 18] + [0] * 18) if capabilities else b""
    total = header + descriptor + len(caching) + len(control) + len(caps)
    body = bytearray(total)
    body[2 if ten else 0] = 0x80
    body[3 if ten else 2] = 0x80
    if ten:
        body[0:2] = (total - 2).to_bytes(2, "big")
        body[3] = 0x80
        body[6:8] = descriptor.to_bytes(2, "big")
    else:
        body[0] = total - 1
        body[3] = descriptor
    offset = header
    if descriptor:
        body[offset + 1:offset + 4] = (0x1266).to_bytes(3, "big")
        body[offset + 5:offset + 8] = (0x200).to_bytes(3, "big")
        offset += descriptor
    body[offset:offset + len(caching)] = caching
    offset += len(caching)
    body[offset:offset + len(control)] = control
    offset += len(control)
    body[offset:offset + len(caps)] = caps
    return bytes(body)


def self_test_modesense_flow():
    """Drive cmd_modesense end to end against a fake SCSI device.

    The command has to survive every combination a broken data stage can
    produce (real sense, transport error, CSW consumed as payload, GOOD with a
    zeroed buffer) without raising: a probe that dies mid-run hides the very
    facts it was written to collect.
    """
    import contextlib
    import io

    saved = {}
    for name in ("open_scsi", "sg_io"):
        saved[name] = globals()[name]

    def sense(key, asc):
        sb = bytearray(18)
        sb[0] = 0x70
        sb[2] = key
        sb[7] = 10
        sb[12] = asc
        return bytes(sb)

    def make_sg_io(broken):
        def fake_sg_io(fd, cdb, data_len=0, direction=SG_DXFER_NONE, timeout_ms=0):
            status, sb, payload, duration = 0, bytes(18), b"", 1
            op = cdb[0]
            if op == 0x1A or op == 0x5A:
                ten = op == 0x5A
                pc = cdb[2] >> 6
                page = cdb[2] & 0x3F
                dbd = bool(cdb[1] & 0x08)
                if pc == 1 or page == 0x02:
                    if broken:
                        payload = b"USBS" + bytes(9)   # CSW eaten as data
                    else:
                        status, sb = 2, sense(5, 0x24)
                elif page == 0x00:
                    payload = bytes([3, 0, 0x80, 0])
                else:
                    payload = _fake_mode_body(dbd, ten, capabilities=True)
            elif op == 0x28:
                status, sb = 2, sense(5, 0x21)
            elif op == 0xFF:
                status, sb = 2, sense(5, 0x20)
            globals().update(LAST_SCSI_STATUS=status, LAST_HOST=0, LAST_DRIVER=0,
                             LAST_RESID=0, LAST_DURATION=duration,
                             LAST_SB_LEN=len(sb) if status else 0)
            return status, sb, payload, 0
        return fake_sg_io

    globals()["open_scsi"] = lambda dev: (os.open(os.devnull, os.O_RDONLY), "/dev/fake")
    try:
        globals()["sg_io"] = make_sg_io(broken=False)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            healthy = cmd_modesense("/dev/fake")
        text = out.getvalue()
        assert healthy == 0, text
        for want in ("mode6_pc1.status=check_condition",
                     "mode6_bad_page.status=check_condition",
                     "mode6_capabilities.status=ok",
                     "mode6_capabilities.page_length=18",
                     "badread.status=check_condition",
                     "badop.status=check_condition"):
            assert want in text, (want, text)

        globals()["sg_io"] = make_sg_io(broken=True)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            broken_rc = cmd_modesense("/dev/fake")
        text = out.getvalue()
        assert broken_rc != 0, text
        line = [l for l in text.splitlines() if l.startswith("mode6_pc1.scsi_status=")]
        assert line and "csw_in_data=1" in line[0], text
        assert "mode6_pc1.status=invalid:" in text, text
    finally:
        globals().update(saved)


def cmd_self_test():
    assert ctypes.sizeof(SgIoHdr) in (88, 80), ctypes.sizeof(SgIoHdr)
    assert cdb_inquiry() == bytes([0x12, 0, 0, 0, 36, 0])
    assert cdb_mode_sense6(0x3F) == bytes([0x1A, 0x08, 0x3F, 0, 0x3F, 0])
    assert cdb_mode_sense6(0x08, 2, 0, 0x24) == bytes([0x1A, 0x00, 0x88, 0, 0x24, 0])
    assert cdb_mode_sense10(0x3F) == bytes([0x5A, 0x08, 0x3F, 0, 0, 0, 0, 0x00, 0x3C, 0])
    assert cdb_read_toc(0, 0, 0x40)[7:9] == b"\x00\x40"
    assert cdb_start_stop(0, 1) == bytes([0x1B, 0, 0, 0, 2, 0])
    inquiry = bytearray(36)
    inquiry[0] = 5
    inquiry[1] = 0x80
    inquiry[2] = 5
    inquiry[4] = 31
    inquiry[8:16] = b"GBL     "
    inquiry[16:32] = b"ISO IMAGE       "
    parsed = parse_inquiry(bytes(inquiry))
    assert parsed["type"] == 5 and parsed["rmb"] == 1 and parsed["product"] == "ISO IMAGE"
    capacity = parse_capacity(bytes([0, 0, 0x0B, 0xFF, 0, 0, 0x08, 0]))
    assert capacity["last_lba"] == 3071 and capacity["block_size"] == 2048
    body = bytearray(42)
    body[0] = 41
    body[2] = 0x80
    body[3] = 8
    body[5:8] = bytes([0, 0x08, 0])
    body[8] = 0
    body[9:12] = bytes([0, 0x02, 0])
    body[12] = 0x08
    body[13] = 18
    body[32] = 0x0A
    body[33] = 8
    body[34] = 0x04
    padded = bytes(body) + bytes(21)          # allocation padding must be ignored
    assert parse_mode_sense(padded, False)["pages"] == "08,0a"
    try:
        parse_mode_sense(bytes(63), False)     # all-zero buffer is not a valid reply
        raise AssertionError("zeroed MODE SENSE buffer must be rejected")
    except ProbeError:
        pass
    parsed = parse_mode_sense(bytes(body), False)
    assert parsed["wp"] == 1 and parsed["block_descriptor_length"] == 8
    assert parsed["block_count"] == 2048 and parsed["logical_block_length"] == 512
    assert parsed["pages"] == "08,0a"
    assert parsed["caching_wce"] == 0 and parsed["control_gltsd"] == 1 and parsed["control_dsense"] == 0
    toc = bytearray(12)
    toc[1] = 10
    toc[2] = 1
    toc[3] = 1
    toc[5] = 0x14
    toc[6] = 1
    parsed = parse_toc(bytes(toc))
    assert parsed["first_track"] == 1 and parsed["last_track"] == 1
    assert parsed["tracks"][0]["number"] == 1 and parsed["tracks"][0]["address"] == 0
    # CDB builders for the write and parameter-list paths.
    assert cdb_write6(0x0123, 9) == bytes([0x0A, 0x00, 0x01, 0x23, 9, 0])
    assert cdb_write6(0, 256)[4] == 0                    # 256 blocks encodes as 0
    assert cdb_write12(0x01020304, 0x0102) == bytes([0xAA, 0, 1, 2, 3, 4, 0, 0, 0, 1, 2, 0])
    w16 = cdb_write16(0x1122334455667788, 0x00000003)
    assert w16[0] == 0x8A
    assert w16[2:10] == bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88])
    assert w16[10:14] == bytes([0, 0, 0, 3])
    assert cdb_mode_select6(24) == bytes([0x15, 0, 0, 0, 24, 0])
    assert cdb_mode_select10(24) == bytes([0x55, 0, 0, 0, 0, 0, 0, 0x00, 24, 0])
    lst = mode_select_list(caching_page(0), ten=False)
    assert lst[0] == len(lst) - 1 and lst[3] == 0 and lst[4] == 0x08 and lst[5] == 18
    lst10 = mode_select_list(caching_page(0), ten=True)
    assert len(lst10) == 8 + 20 and lst10[6:8] == bytes([0, 0]) and lst10[8] == 0x08
    assert caching_page(1)[2] & 0x04 == 0x04 and caching_page(0)[2] & 0x04 == 0x00
    # Page lengths for the error-recovery (0x01) and informational (0x1c) pages
    # must be parsed out so a self-test run can assert them.
    hdr = bytearray(4)
    hdr[2] = 0x80
    pages = (bytes([0x01, 10]) + bytes(10) + bytes([0x08, 18]) + bytes(18)
             + bytes([0x0A, 8, 0x04]) + bytes(7) + bytes([0x1C, 8]) + bytes(8))
    body2 = hdr + pages
    body2[0] = (len(body2) - 1) & 0xFF
    parsed = parse_mode_sense(bytes(body2), False)
    assert parsed["pages"] == "01,08,0a,1c", parsed["pages"]
    assert parsed["recovery_page_length"] == 10 and parsed["informational_page_length"] == 8
    # A 64-byte reply (header + 8-byte block descriptor + the four pages) must
    # parse when the declared length exactly fills the buffer; a narrower
    # allocation would truncate it and look like a short read.
    body3 = bytearray(4 + 8 + 12 + 20 + 10 + 10)
    body3[2] = 0x80
    body3[3] = 8
    body3[5:8] = (2048).to_bytes(3, "big")
    body3[9:12] = (512).to_bytes(3, "big")
    cursor = 4 + 8
    for code, size in ((0x01, 10), (0x08, 18), (0x0A, 8), (0x1C, 8)):
        body3[cursor] = code
        body3[cursor + 1] = size
        cursor += 2 + size
    body3[0] = len(body3) - 1
    assert cursor == len(body3)
    parsed3 = parse_mode_sense(bytes(body3), False)
    assert parsed3["block_descriptor_length"] == 8, parsed3["block_descriptor_length"]
    assert parsed3["block_count"] == 2048 and parsed3["logical_block_length"] == 512
    assert parsed3["pages"] == "01,08,0a,1c", parsed3["pages"]

    self_test_sgio_header()
    self_test_modesense_flow()
    print("self-test=ok")


def main(argv):
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    command = argv[1]
    if command == "self-test":
        cmd_self_test()
        return 0
    if len(argv) < 3:
        print("%s requires a device" % command, file=sys.stderr)
        return 2
    dev = argv[2]
    handlers = {
        "info": cmd_info,
        "modesense": cmd_modesense,
        "mmc": cmd_mmc,
        "modeselect": cmd_modeselect,
        "write": cmd_write,
        "eject": cmd_eject,
        "openrw": cmd_openrw,
    }
    if command not in handlers:
        print("unknown command: %s" % command, file=sys.stderr)
        return 2
    try:
        if command == "write":
            return cmd_write(dev, argv[3:]) or 0
        return handlers[command](dev) or 0
    except ProbeError as exc:
        print("error=%s" % exc, file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv))
