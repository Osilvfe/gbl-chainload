#!/usr/bin/env python3
"""msc_probe.py - inspect a GBL USB MSC LUN through SG_IO.

Standard library only. All output is ``key=value`` lines so shell callers can
grep/compare without extra tooling.

    info <dev>        INQUIRY + READ CAPACITY(10)
    modesense <dev>   MODE SENSE(6)/(10), page 0x3f and the negative cases
    mmc <dev>         GET CONFIGURATION + READ TOC (CD-ROM LUN)
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


def sg_io(fd, cdb, data_len=0, direction=SG_DXFER_NONE, timeout_ms=20000):
    global LAST_HOST, LAST_DRIVER, LAST_RESID, LAST_SCSI_STATUS, LAST_DURATION
    global LAST_SB_LEN, LAST_DATA
    data = ctypes.create_string_buffer(max(data_len, 1))
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
    if ten:
        status, sb, data, _ = sg_io(fd, cdb_mode_sense10(page, pc, dbd), 0x3C, SG_DXFER_FROM_DEV)
    else:
        status, sb, data, _ = sg_io(fd, cdb_mode_sense6(page, pc, dbd), 0x3F, SG_DXFER_FROM_DEV)
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


def _fake_mode_body(dbd, ten=False):
    """MODE SENSE reply shaped like the firmware's: pages 0x08 and 0x0a."""
    header = 8 if ten else 4
    descriptor = 0 if dbd else 8
    caching = bytes([0x08, 18, 0x00] + [0] * 17)
    control = bytes([0x0A, 8, 0x04] + [0] * 7)
    total = header + descriptor + len(caching) + len(control)
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
                    payload = _fake_mode_body(dbd, ten)
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
        "eject": cmd_eject,
        "openrw": cmd_openrw,
    }
    if command not in handlers:
        print("unknown command: %s" % command, file=sys.stderr)
        return 2
    try:
        return handlers[command](dev) or 0
    except ProbeError as exc:
        print("error=%s" % exc, file=sys.stderr)
        return 4


if __name__ == "__main__":
    sys.exit(main(sys.argv))
