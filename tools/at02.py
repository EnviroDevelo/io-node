#!/usr/bin/env python3
"""at02.py - AT-02 conformance test for the CEMS I/O board node: the register rules, from a PC.

Contract: "CEMS I/O Module Requirements: Firmware and Hardware", AT-02:
  Reserved addresses read 0; writes to read-only registers give exception 02; out-of-range values
  give 03 and change nothing; unknown functions give 01; another address or a bad CRC gets no
  reply, and RX_ERRORS counts the bad CRC.
plus the identification block (register map v1) and the node's request framing rules.

Wiring: a USB-RS485 adapter on the board's bus terminals, the main board DISCONNECTED (two masters
on one bus collide). 115200 baud, 8N1. Or the board's own USB-C port with --usb.

  python at02.py COM5 --address 3            RS-485, board address 3 (set with set_address.py)
  python at02.py COM7 --usb                  USB-C port (answers at 247)
  python at02.py /dev/ttyUSB0 -a 3 -v        Linux, show every frame

Needs Python 3.8+ and pyserial (pip install pyserial). Without pyserial it falls back to plain
POSIX serial (Linux / macOS only).

Safety: the script never switches anything on. The only output write it makes is all-off
(OUT_CMD = 0, RELAY_CMD = 0), which turns off anything that was on. Settings it changes
(HOLD_DS, FILTER_MS, WB_ENABLE, OL_ENABLE) are put back at the end. It never writes the bootloader
key or anything to FAULT_CLEAR (it would clear latched faults).

Exit code 0 when every check passes, 1 when any fails, 2 when the board cannot be reached.
"""
import argparse
import datetime
import os
import sys
import time

# ---- register map v1 (platform/include/iobus.h) ----
R_OUT_CMD, R_RELAY_CMD, R_CMD_SEQ = 0, 1, 2
R_STATUS, R_OUT_APPLIED, R_RELAY_APPLIED, R_CMD_ECHO, R_RX_ERRORS = 16, 18, 19, 25, 26
R_HOLD_DS, R_FILTER_MS, R_WB_ENABLE, R_OL_ENABLE, R_FAULT_CLEAR, R_BOOTLOADER, R_NODE_ADDR = 32, 33, 34, 35, 36, 37, 38
R_MAGIC, R_MAP_VERSION, R_HW_REV, R_FW_VERSION, R_FW_BUILD, R_UID = 64, 65, 66, 67, 68, 69
R_N_OUT_A, R_N_OUT_B, R_N_RELAYS, R_N_INPUTS = 75, 76, 77, 78
REG_SPACE = 80
MAGIC, MAP_VERSION, BOOT_KEY = 0x494F, 1, 0xB007
USB_ADDRESS = 247
WRITABLE = {0, 1, 2, 32, 33, 34, 35, 36}            # 37 too, but over USB only and only the key
DEFINED = set(range(0, 3)) | set(range(16, 28)) | set(range(32, 39)) | set(range(64, 79))
RESERVED = [r for r in range(REG_SPACE) if r not in DEFINED]
READ_ZERO = RESERVED + [R_FAULT_CLEAR, R_BOOTLOADER]   # write-only registers read 0 as well


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


def frame(*parts):
    body = bytes(parts[0]) if len(parts) == 1 else bytes(parts)
    c = crc16(body)
    return body + bytes([c & 0xFF, c >> 8])


def u16(*vals):
    out = []
    for v in vals:
        out += [(v >> 8) & 0xFF, v & 0xFF]
    return out


# ---------------------------------------------------------------- serial link
class Link:
    """Half-duplex request/reply. A reply ends at a gap of `gap` seconds after its last byte."""

    def __init__(self, port, baud, timeout, gap, verbose):
        self.timeout, self.gap, self.verbose = timeout, gap, verbose
        try:
            import serial  # pyserial
            self.ser = serial.Serial(port, baud, bytesize=8, parity="N", stopbits=1, timeout=0)
            self._read = lambda: self.ser.read(512)
            self._write = self.ser.write
            self._flush = self.ser.reset_input_buffer
            self.close = self.ser.close
        except ImportError:
            self._posix_open(port, baud)

    def _posix_open(self, port, baud):
        import termios
        import tty
        fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        a = termios.tcgetattr(fd)
        speed = getattr(termios, "B%d" % baud, None)
        if speed is not None:
            a[4] = a[5] = speed
        a[2] &= ~(termios.PARENB | termios.CSTOPB)
        termios.tcsetattr(fd, termios.TCSANOW, a)

        def rd():
            try:
                return os.read(fd, 512)
            except BlockingIOError:
                return b""
        self._read = rd
        self._write = lambda b: os.write(fd, b)
        self._flush = lambda: termios.tcflush(fd, termios.TCIFLUSH)
        self.close = lambda: os.close(fd)

    def xfer(self, req, timeout=None):
        """Send req; return the reply bytes (b'' = none within the timeout)."""
        timeout = self.timeout if timeout is None else timeout
        time.sleep(0.005)                              # bus idle before the request (>= 3.5 chars)
        self._flush()
        self._write(req)
        t0 = time.monotonic()
        buf, last = b"", None
        while True:
            now = time.monotonic()
            chunk = self._read()
            if chunk:
                buf += chunk
                last = now
            elif buf and now - last >= self.gap:
                break
            elif not buf and now - t0 >= timeout:
                break
            time.sleep(0.0005)
        if self.verbose:
            print("    -> %s" % req.hex(" "))
            print("    <- %s" % (buf.hex(" ") if buf else "(no reply)"))
        return buf


# ---------------------------------------------------------------- Modbus client
class Reply:
    def __init__(self, raw, addr, fc):
        self.raw, self.ok, self.exc, self.data, self.problem = raw, False, None, b"", None
        if not raw:
            self.problem = "no reply"
        elif len(raw) < 5:
            self.problem = "short reply (%d bytes)" % len(raw)
        elif crc16(raw[:-2]) != (raw[-2] | raw[-1] << 8):
            self.problem = "reply CRC wrong"
        elif raw[0] != addr:
            self.problem = "reply from address %d" % raw[0]
        elif raw[1] == fc | 0x80:
            self.exc = raw[2]
            if len(raw) != 5:
                self.problem = "exception reply of %d bytes" % len(raw)
        elif raw[1] != fc:
            self.problem = "reply function %d" % raw[1]
        else:
            self.ok, self.data = True, raw[2:-2]

    def regs(self):
        n = self.data[0] // 2
        return [self.data[1 + 2 * i] << 8 | self.data[2 + 2 * i] for i in range(n)]

    def what(self):
        if self.problem:
            return self.problem
        if self.exc is not None:
            return "exception %02d" % self.exc
        return "normal reply"


class Node:
    def __init__(self, link, addr):
        self.link, self.addr = link, addr

    def req(self, pdu, addr=None, timeout=None):
        a = self.addr if addr is None else addr
        return Reply(self.link.xfer(frame([a] + list(pdu)), timeout), a, pdu[0])

    def read(self, start, count, fc=3):
        return self.req([fc] + u16(start, count))

    def write1(self, reg, val):
        return self.req([6] + u16(reg, val))

    def writen(self, start, vals):
        return self.req([16] + u16(start, len(vals)) + [2 * len(vals)] + u16(*vals))

    def rw(self, rstart, rcount, wstart, wvals):
        return self.req([23] + u16(rstart, rcount, wstart, len(wvals)) + [2 * len(wvals)] + u16(*wvals))

    def regs(self, start, count):
        r = self.read(start, count)
        if not r.ok:
            raise IOError("read %d..%d: %s" % (start, start + count - 1, r.what()))
        v = r.regs()
        if len(v) != count:
            raise IOError("read %d..%d: %d registers returned" % (start, start + count - 1, len(v)))
        return v


# ---------------------------------------------------------------- the checks
class Run:
    def __init__(self, log):
        self.passed = self.failed = 0
        self.log = log
        self.failures = []

    def section(self, title):
        self.out("\n%s" % title)

    def out(self, s):
        print(s)
        if self.log:
            self.log.write(s + "\n")

    def check(self, ok, text, detail=""):
        if ok:
            self.passed += 1
            self.out("  PASS  %s" % text)
        else:
            self.failed += 1
            self.failures.append(text)
            self.out("  FAIL  %s%s" % (text, ("  [" + detail + "]") if detail else ""))
        return ok


def expect_exc(run, r, code, text):
    return run.check(r.exc == code and not r.problem, text, "got %s" % r.what())


def expect_none(run, r, text):
    return run.check(not r.raw, text, "got %d bytes: %s" % (len(r.raw), r.raw.hex(" ")))


def main():
    ap = argparse.ArgumentParser(description="AT-02 register-rule conformance test for the CEMS I/O board node.")
    ap.add_argument("port", help="serial port: COM5, /dev/ttyUSB0 ...")
    ap.add_argument("-a", "--address", type=int, help="node address (1..247, see set_address.py); with --usb default 247")
    ap.add_argument("--usb", action="store_true", help="the board's USB-C port instead of RS-485")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-t", "--timeout", type=float, default=0.2, help="reply timeout, s (default 0.2)")
    ap.add_argument("--gap", type=float, default=0.01, help="end-of-reply silence, s (default 0.01)")
    ap.add_argument("--log", help="also write the report to this file (default at02-<time>.txt; '-' for none)")
    ap.add_argument("-v", "--verbose", action="store_true", help="print every frame")
    args = ap.parse_args()

    if args.address is None:
        if not args.usb:
            ap.error("--address is required on RS-485 (the board's address, 1..247; set_address.py shows it)")
        args.address = USB_ADDRESS
    if not 1 <= args.address <= 247:
        ap.error("address must be 1..247")
    logname = args.log or datetime.datetime.now().strftime("at02-%Y%m%d-%H%M%S.txt")
    log = None if logname == "-" else open(logname, "w")

    run = Run(log)
    link = Link(args.port, args.baud, args.timeout, args.gap, args.verbose)
    node = Node(link, args.address)
    usb = args.usb
    run.out("AT-02 register rules  %s  port %s  %s  address %d" % (
        datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), args.port, "USB" if usb else "RS-485 %d 8N1" % args.baud,
        args.address))

    # ---- reach the node, save its settings ----
    try:
        ident = node.regs(64, 15)
        saved = node.regs(R_HOLD_DS, 4)
    except IOError as e:
        run.out("\nCannot reach the node: %s" % e)
        run.out("Check the port, the address (set_address.py over USB shows it), A/B wiring, termination and that the main board is disconnected.")
        return 2

    try:
        result = checks(run, node, usb, ident, saved)
    finally:
        # ---- put the settings back ----
        r = node.writen(R_HOLD_DS, saved)
        if not r.ok:
            run.out("\nWARNING: could not restore settings 32-35 to %s (%s)" % (saved, r.what()))
        link.close()
    run.out("\n%d passed, %d failed" % (run.passed, run.failed))
    if run.failed:
        run.out("Failed:\n  " + "\n  ".join(run.failures))
    run.out("AT-02 %s" % ("PASSED" if not run.failed and result else "FAILED"))
    if log:
        log.close()
        print("Report written to %s" % logname)
    return 0 if not run.failed and result else 1


def checks(run, node, usb, ident, saved):
    hold, filt, wb, ol = saved

    # ---------------- identification ----------------
    run.section("Identification (registers 64-78)")
    run.check(ident[0] == MAGIC, "MAGIC = 0x494F", "0x%04X" % ident[0])
    run.check(ident[1] == MAP_VERSION, "MAP_VERSION = 1", str(ident[1]))
    run.check(ident[11:15] == [8, 8, 2, 8], "channel counts 8 / 8 / 2 / 8", str(ident[11:15]))
    fw = ident[3]
    run.out("  info  HW_REV %d, firmware %d.%d build %d, UID %s" % (
        ident[2], fw >> 8, fw & 0xFF, ident[4], " ".join("%04X" % u for u in ident[5:11])))

    # ---------------- reads ----------------
    run.section("Reads")
    r3 = node.read(0, REG_SPACE, 3)
    r4 = node.read(0, REG_SPACE, 4)
    if run.check(r3.ok and len(r3.regs()) == REG_SPACE, "FC03 reads all 80 registers in one request", r3.what()):
        v = r3.regs()
        bad = [r for r in READ_ZERO if v[r] != 0]
        run.check(not bad, "reserved addresses (%d of them) and FAULT_CLEAR / BOOTLOADER read 0" % len(READ_ZERO),
                  "non-zero: " + ", ".join("%d=0x%04X" % (r, v[r]) for r in bad))
        if run.check(r4.ok and len(r4.regs()) == REG_SPACE, "FC04 reads the same register space", r4.what()):
            stable = READ_ZERO + list(range(32, 36)) + [R_NODE_ADDR] + list(range(64, 79))
            diff = [r for r in stable if r3.regs()[r] != r4.regs()[r]]
            run.check(not diff, "FC03 and FC04 agree", "differ at " + str(diff))
    for fc in (3, 4):
        expect_exc(run, node.read(REG_SPACE - 1, 2, fc), 2, "FC%02d past register 79 (79, 2 registers) -> 02" % fc)
        expect_exc(run, node.read(200, 1, fc), 2, "FC%02d at register 200 -> 02" % fc)
        expect_exc(run, node.read(0, 0, fc), 3, "FC%02d count 0 -> 03" % fc)
        expect_exc(run, node.read(0, 126, fc), 3, "FC%02d count 126 -> 03" % fc)

    # ---------------- writes to read-only registers ----------------
    run.section("Writes to read-only and reserved registers -> 02")
    for reg, name in ((R_STATUS, "STATUS"), (R_OUT_APPLIED, "OUT_APPLIED"), (R_CMD_ECHO, "CMD_ECHO"),
                      (R_RX_ERRORS, "RX_ERRORS"), (R_MAGIC, "MAGIC"), (R_N_INPUTS, "N_INPUTS"),
                      (3, "reserved 3"), (40, "reserved 40"), (79, "reserved 79")):
        expect_exc(run, node.write1(reg, 0), 2, "FC06 %s (%d)" % (name, reg))
    expect_exc(run, node.write1(REG_SPACE, 0), 2, "FC06 register 80 (outside the map)")
    r = node.writen(30, [0, 0, hold + 1 if hold < 6000 else hold - 1])
    expect_exc(run, r, 2, "FC16 30..32 (reserved 30, 31 then HOLD_DS) -> 02")
    run.check(node.regs(R_HOLD_DS, 1)[0] == hold, "  ... and HOLD_DS unchanged (the whole request refused)")
    expect_exc(run, node.writen(78, [0, 0]), 2, "FC16 78..79 (read-only) -> 02")
    expect_exc(run, node.writen(79, [0, 0]), 2, "FC16 79..80 (past the map) -> 02")
    expect_exc(run, node.rw(R_STATUS, 12, R_STATUS, [0]), 2, "FC23 writing STATUS -> 02")
    own = node.regs(R_NODE_ADDR, 1)[0]
    if usb:
        expect_exc(run, node.write1(R_BOOTLOADER, 0x1234), 3, "FC06 BOOTLOADER with a wrong key over USB -> 03")
        expect_exc(run, node.write1(R_NODE_ADDR, 248), 3, "FC06 NODE_ADDRESS 248 over USB -> 03")
        run.out("  info  the board's own address (register 38): %s" % (own if own else "not set"))
    else:
        expect_exc(run, node.write1(R_BOOTLOADER, BOOT_KEY), 2, "FC06 BOOTLOADER over RS-485, even with the key -> 02")
        expect_exc(run, node.write1(R_NODE_ADDR, own), 2, "FC06 NODE_ADDRESS over RS-485 -> 02 (USB only)")
        run.check(own == node.addr, "NODE_ADDRESS (38) reads the address it answers at", "reads %d" % own)
    run.check(node.regs(R_NODE_ADDR, 1)[0] == own, "  ... NODE_ADDRESS unchanged")

    # ---------------- out-of-range values ----------------
    run.section("Out-of-range values -> 03, nothing changed")
    before = node.regs(R_HOLD_DS, 4)
    cases = [(R_HOLD_DS, 0, "HOLD_DS 0"), (R_HOLD_DS, 6001, "HOLD_DS 6001"), (R_HOLD_DS, 0xFFFF, "HOLD_DS 65535"),
             (R_FILTER_MS, 101, "FILTER_MS 101"), (R_WB_ENABLE, 256, "WB_ENABLE 256"),
             (R_OL_ENABLE, 256, "OL_ENABLE 256"), (R_FAULT_CLEAR, 4, "FAULT_CLEAR 4"),
             (R_RELAY_CMD, 4, "RELAY_CMD 4")]
    for reg, val, name in cases:
        expect_exc(run, node.write1(reg, val), 3, "FC06 %s" % name)
    run.check(node.regs(R_HOLD_DS, 4) == before, "  ... settings 32-35 unchanged after all of them")
    new_hold = hold + 1 if hold < 6000 else hold - 1
    expect_exc(run, node.writen(R_HOLD_DS, [new_hold, filt, wb, 256]), 3,
               "FC16 32..35 with a good HOLD_DS and a bad OL_ENABLE -> 03")
    after = node.regs(R_HOLD_DS, 4)
    run.check(after == before, "  ... HOLD_DS not written (the whole request refused)", "%s -> %s" % (before, after))
    relay = node.regs(R_RELAY_CMD, 1)[0]
    expect_exc(run, node.writen(R_OUT_CMD, [0, 4, 0]), 3, "FC16 0..2 with RELAY_CMD 4 -> 03")
    run.check(node.regs(R_RELAY_CMD, 1)[0] == relay, "  ... RELAY_CMD unchanged")

    # ---------------- unknown functions ----------------
    run.section("Unknown functions -> 01")
    for fc, pdu, name in ((1, u16(0, 8), "01 read coils"), (2, u16(0, 8), "02 read discrete inputs"),
                          (5, u16(0, 0xFF00), "05 write coil"), (8, u16(0, 0x1234), "08 diagnostics"),
                          (15, u16(0, 8) + [1, 0], "15 write coils"), (17, [], "17 report server ID"),
                          (22, u16(32, 0xFFFF, 0), "22 mask write"), (43, [14, 1, 0], "43 device identification"),
                          (100, [], "100 user-defined")):
        expect_exc(run, node.req([fc] + pdu), 1, "FC%s" % name)

    # ---------------- malformed requests ----------------
    run.section("Malformed requests -> 03")
    expect_exc(run, node.req([3] + u16(0, 1) + [0]), 3, "FC03 with an extra byte")
    expect_exc(run, node.req([16] + u16(R_HOLD_DS, 2) + [2] + u16(hold)), 3, "FC16 byte count does not match the count")
    expect_exc(run, node.req([16] + u16(R_HOLD_DS, 0) + [0]), 3, "FC16 count 0")
    expect_exc(run, node.req([6] + u16(R_HOLD_DS)), 3, "FC06 without its value")

    # ---------------- addressing and CRC ----------------
    run.section("Another address, broadcast and bad CRC -> no reply")
    errs0 = node.regs(R_RX_ERRORS, 1)[0]
    if usb:                                            # USB also answers at the board's own address
        own = node.regs(R_NODE_ADDR, 1)[0]
        others = [a for a in (16, 200, 246, 9) if a != own][:3]
    else:                                              # 247 is the USB service address only
        others = [a for a in (node.addr % 247 + 1, 200, 246, USB_ADDRESS) if a != node.addr]
    for a in others:
        expect_none(run, node.req([3] + u16(R_MAGIC, 1), addr=a), "FC03 to address %d" % a)
    expect_none(run, node.req([6] + u16(R_HOLD_DS, hold), addr=0), "FC06 broadcast (address 0)")
    good = frame([node.addr, 3] + u16(R_MAGIC, 1))
    bad_crc = good[:-1] + bytes([good[-1] ^ 0xFF])
    expect_none(run, Reply(node.link.xfer(bad_crc), node.addr, 3), "FC03 with a bad CRC")
    bad_body = bytearray(good)
    bad_body[3] ^= 0x01                                # a bit flipped in transit, CRC now wrong
    expect_none(run, Reply(node.link.xfer(bytes(bad_body)), node.addr, 3), "FC03 with a flipped data bit")
    errs1 = node.regs(R_RX_ERRORS, 1)[0]
    if usb:
        run.out("  info  RX_ERRORS %d -> %d (counts RS-485 errors only; not checked over USB)" % (errs0, errs1))
    else:
        run.check((errs1 - errs0) & 0xFFFF == 2, "RX_ERRORS counted exactly the 2 bad-CRC frames",
                  "%d -> %d" % (errs0, errs1))

    # ---------------- accepted writes ----------------
    run.section("Accepted writes (all-off only)")
    r = node.write1(R_HOLD_DS, hold)
    run.check(r.ok and r.raw[:6] == frame([node.addr, 6] + u16(R_HOLD_DS, hold))[:6], "FC06 HOLD_DS echoes the request", r.what())
    r = node.writen(R_HOLD_DS, [hold, filt, wb, ol])
    run.check(r.ok and list(r.raw[2:6]) == u16(R_HOLD_DS, 4), "FC16 32..35 answers start and count", r.what())
    seq = int(time.time()) & 0xFFFF
    r = node.rw(R_STATUS, 12, R_OUT_CMD, [0, 0, seq])
    if usb and r.exc == 4:
        run.out("  info  FC23 all-off over USB refused with 04: the RS-485 master is in control (FW-56)")
    elif run.check(r.ok and len(r.regs()) == 12, "FC23 writes all-off and reads the status block", r.what()):
        st = r.regs()
        run.check(st[R_CMD_ECHO - 16] == seq, "  ... CMD_ECHO = CMD_SEQ in the same reply (write before read)",
                  "echo %d, seq %d" % (st[R_CMD_ECHO - 16], seq))
        run.check(st[R_OUT_APPLIED - 16] == 0 and st[R_RELAY_APPLIED - 16] == 0, "  ... OUT_APPLIED and RELAY_APPLIED 0")
        run.check(st[0] & 0x0001, "  ... STATUS.OUT_ENABLED set (gate open after a write of register 0)",
                  "STATUS 0x%04X" % st[0])
    return True


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
