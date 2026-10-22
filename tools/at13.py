#!/usr/bin/env python3
"""at13.py - AT-13 bench test for the CEMS I/O board node: the 8 digital inputs, from a PC.

Contract: "CEMS I/O Module Requirements: Firmware and Hardware", AT-13:
  Inputs: each one on and off; a 5 ms and a 12 ms pulse with the 8 ms filter; cut the wire of a
  channel with its wire-break resistor fitted. Pass when each input follows; the 5 ms pulse is
  ignored and the 12 ms one seen; "In n: wire break" only for channels with the check on.

The script reads register 17 (INPUTS) as fast as the link allows and guides you through the
steps; you switch the inputs. It does not wait for Enter between steps: it sees what you did.

  python at13.py COM7 --usb                       the board's USB-C port (recommended: fastest polling)
  python at13.py COM5 --address 3                 USB-RS485 adapter, main board disconnected
  python at13.py COM7 --usb --loopback            no pulse source needed (see below)

Pulses: a function generator, a PLC output or any 24 V source you can pulse for 5 ms and 12 ms,
into input 1 (--pulse-input to change). Or --loopback: wire bank B outputs 9-16 to inputs 1-8
(output 9 to input 1 and so on, field 24 V B on); the script then switches the inputs itself
through those outputs and times the pulses on the PC. Loopback is the only mode that switches
outputs on, and only bank B; it needs the RS-485 master not in control (main board disconnected
or the board's hold time run out) and turns everything off at the end.

Polling speed: a 12 ms pulse is visible in register 17 for about 12 ms (the chip's filter delays
both edges by its 10 ms filter time). The script measures how fast it can poll and warns when it
is too slow to be sure of seeing it. Over the USB-C port it polls about every 1-2 ms. Through a
USB-RS485 adapter, set the adapter's latency timer to 1 ms (FTDI: Device Manager, Port Settings,
Advanced; default 16 ms).

Settings it changes (FILTER_MS, WB_ENABLE) are put back at the end. Needs Python 3.8+ and
pyserial. Exit code 0 when every check passes, 1 when any fails, 2 when it cannot start.
"""
import argparse
import datetime
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from at02 import Link, Node, Run, frame, u16, crc16, USB_ADDRESS  # noqa: E402

R_OUT_CMD, R_STATUS, R_INPUTS, R_WIREBREAK = 0, 16, 17, 24
R_HOLD_DS, R_FILTER_MS, R_WB_ENABLE = 32, 33, 34
ST_FIELD_A_OK, ST_IN_CHIP = 0x0004, 0x0040
FILTER_MS = 8                       # AT-13 uses the default filter
POLL_OK_MS = 4.0                    # polling slower than this cannot be sure of a 12 ms pulse


class Fast:
    """Back-to-back single-register reads and writes: no idle wait, the reply ends at its length."""

    def __init__(self, node, timeout=0.1):
        self.node, self.link, self.timeout = node, node.link, timeout

    def _xfer(self, req, length):
        self.link._flush()
        self.link._write(req)
        t0 = time.perf_counter()
        buf = b""
        while time.perf_counter() - t0 < self.timeout:
            buf += self.link._read()
            if len(buf) >= 5 and buf[1] & 0x80:
                return buf[:5]
            if len(buf) >= length:
                return buf[:length]
        return buf

    def read(self, reg):
        """The register's value, or None if the read failed."""
        a = self.node.addr
        r = self._xfer(frame([a, 3] + u16(reg, 1)), 7)
        if len(r) != 7 or r[0] != a or r[1] != 3 or crc16(r[:5]) != (r[5] | r[6] << 8):
            return None
        return r[3] << 8 | r[4]

    def write(self, reg, val):
        """None if accepted, else the exception code (or -1: no valid reply)."""
        a = self.node.addr
        r = self._xfer(frame([a, 6] + u16(reg, val)), 8)
        if len(r) == 5 and r[1] == 0x86:
            return r[2]
        return None if len(r) == 8 and r[:6] == frame([a, 6] + u16(reg, val))[:6] else -1


def bitlist(mask):
    return ", ".join(str(i + 1) for i in range(8) if mask >> i & 1) or "none"


def watch(fast, until, timeout, on_sample=None):
    """Poll INPUTS until until(value) or timeout; returns (value, seconds) or (last, None)."""
    t0 = time.perf_counter()
    v = None
    while time.perf_counter() - t0 < timeout:
        x = fast.read(R_INPUTS)
        if x is None:
            continue
        v = x
        if on_sample:
            on_sample(v)
        if until(v):
            return v, time.perf_counter() - t0
    return v, None


class PulseCounter:
    """Visible pulses on one input bit, from back-to-back samples."""

    def __init__(self, bit):
        self.bit, self.high_since, self.last_t, self.widths, self.samples = bit, None, None, [], 0
        self.max_gap = 0.0

    def __call__(self, v):
        t = time.perf_counter()
        if self.last_t is not None:
            self.max_gap = max(self.max_gap, t - self.last_t)
        self.last_t = t
        self.samples += 1
        on = bool(v >> self.bit & 1)
        if on and self.high_since is None:
            self.high_since = t
        elif not on and self.high_since is not None:
            self.widths.append((t - self.high_since) * 1000)
            self.high_since = None


def countdown(fast, seconds, counter, stop_after=None):
    t0 = time.perf_counter()
    shown = None
    while True:
        left = seconds - (time.perf_counter() - t0)
        if left <= 0 or (stop_after and len(counter.widths) >= stop_after):
            break
        s = int(left) + 1
        if s != shown:
            print("\r    watching: %2d s left, %d pulses seen  " % (s, len(counter.widths)), end="", flush=True)
            shown = s
        v = fast.read(R_INPUTS)
        if v is not None:
            counter(v)
    print("\r" + " " * 50 + "\r", end="")


def ask_int(prompt, default):
    try:
        s = input("    %s [%d]: " % (prompt, default)).strip()
        return int(s) if s else default
    except (ValueError, EOFError):
        return default


def main():
    ap = argparse.ArgumentParser(description="AT-13 digital input bench test for the CEMS I/O board node.")
    ap.add_argument("port", help="serial port: COM7, /dev/ttyACM0 ...")
    ap.add_argument("-a", "--address", type=int, help="node address (1..247, see set_address.py); with --usb default 247")
    ap.add_argument("--usb", action="store_true", help="the board's USB-C port instead of RS-485")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--loopback", action="store_true", help="bank B outputs 9-16 wired to inputs 1-8")
    ap.add_argument("--pulse-input", type=int, default=1, help="input for the pulse tests (default 1)")
    ap.add_argument("--wb-input", type=int, default=1, help="input whose wire is cut with its check on (default 1)")
    ap.add_argument("--other-input", type=int, default=2, help="input whose wire is cut with its check off (default 2)")
    ap.add_argument("--pulses", type=int, default=10, help="pulses of each length (loopback; default 10)")
    ap.add_argument("--skip", default="", help="steps to skip, e.g. 'wb' or 'each,pulse'")
    ap.add_argument("--log", help="also write the report to this file (default at13-<time>.txt; '-' for none)")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.address is None:
        if not args.usb:
            ap.error("--address is required on RS-485 (the board's address, 1..247; set_address.py shows it)")
        args.address = USB_ADDRESS
    for name in ("pulse_input", "wb_input", "other_input"):
        if not 1 <= getattr(args, name) <= 8:
            ap.error("--%s must be 1..8" % name.replace("_", "-"))
    if args.wb_input == args.other_input:
        ap.error("--wb-input and --other-input must differ")
    skip = set(s.strip() for s in args.skip.split(",") if s.strip())

    logname = args.log or datetime.datetime.now().strftime("at13-%Y%m%d-%H%M%S.txt")
    log = None if logname == "-" else open(logname, "w")
    run = Run(log)
    link = Link(args.port, args.baud, 0.2, 0.01, args.verbose)
    node = Node(link, args.address)
    fast = Fast(node)
    run.out("AT-13 digital inputs  %s  port %s  %s  address %d%s" % (
        datetime.datetime.now().strftime("%Y-%m-%d %H:%M"), args.port, "USB" if args.usb else "RS-485",
        args.address, "  loopback" if args.loopback else ""))

    try:
        saved = node.regs(R_HOLD_DS, 4)
        status = node.regs(R_STATUS, 1)[0]
    except IOError as e:
        run.out("\nCannot reach the node: %s" % e)
        return 2
    if not status & ST_FIELD_A_OK or status & ST_IN_CHIP:
        run.out("\nCannot start: STATUS 0x%04X: %s" % (status, "field 24 V A missing (the inputs need it)"
                if not status & ST_FIELD_A_OK else "the input chip reports a fault (IN_CHIP)"))
        return 2
    ok = False
    try:
        ok = steps(run, node, fast, args, skip, saved)
    finally:
        if args.loopback:
            fast.write(R_OUT_CMD, 0)
        r = node.writen(R_HOLD_DS, saved)
        if not r.ok:
            run.out("\nWARNING: could not restore settings 32-35 to %s (%s)" % (saved, r.what()))
        link.close()
    run.out("\n%d passed, %d failed" % (run.passed, run.failed))
    if run.failures:
        run.out("Failed:\n  " + "\n  ".join(run.failures))
    passed = ok and not run.failed
    run.out("AT-13 %s" % ("PASSED" if passed else "FAILED" if ok else "NOT FINISHED"))
    if log:
        log.close()
        print("Report written to %s" % logname)
    return 0 if passed else 1


def drive(fast, run, mask):
    """Loopback: bank B outputs 9-16 = inputs 1-8."""
    e = fast.write(R_OUT_CMD, mask << 8)
    if e is not None:
        why = {4: "exception 04: the RS-485 master is in control (disconnect the main board, wait for the hold time)",
               -1: "no reply"}.get(e, "exception %02d" % e)
        raise RuntimeError("cannot switch bank B for the loopback: " + why)


def steps(run, node, fast, args, skip, saved):
    lb = args.loopback
    hold, filt, wb, ol = saved
    if node.writen(R_HOLD_DS, [hold, FILTER_MS, 0, ol]).exc is not None:
        run.out("Cannot set FILTER_MS 8 / WB_ENABLE 0")
        return False
    time.sleep(0.1)                                    # the node reconfigures the chip

    # ---- polling speed ----
    run.section("Polling speed")
    t0 = time.perf_counter()
    n = good = 0
    while n < 200:
        n += 1
        good += fast.read(R_INPUTS) is not None
    period = (time.perf_counter() - t0) / n * 1000
    run.out("  info  %d reads, %.2f ms each, %d failed" % (n, period, n - good))
    fast_enough = period <= POLL_OK_MS
    if not fast_enough:
        run.out("  WARN  slower than %.0f ms: a 12 ms pulse may fall between reads. Use the USB-C port, or set the"
                " adapter's latency timer to 1 ms." % POLL_OK_MS)
    if good < n:
        run.check(False, "every read answered", "%d of %d failed" % (n - good, n))

    try:
        # ---- 1: each input on and off ----
        if "each" not in skip:
            run.section("1. Each input on and off (filter %d ms)" % FILTER_MS)
            base, _ = watch(fast, lambda v: True, 1.0)
            if base:
                run.out("  WAIT  inputs on now: %s; switch them all off" % bitlist(base))
                base, t = watch(fast, lambda v: v == 0, 60)
                if t is None:
                    run.check(False, "all inputs off before the step", "still on: " + bitlist(base or 0))
                    return False
            for i in range(8):
                want = 1 << i
                if lb:
                    drive(fast, run, want)
                else:
                    print("    Switch input %d ON" % (i + 1))
                v, t = watch(fast, lambda x: x != 0, 2.0 if lb else 60)
                run.check(v == want, "input %d on: register 17 shows only input %d" % (i + 1, i + 1),
                          "shows %s" % bitlist(v or 0) if t is not None else "nothing within the time")
                if lb:
                    drive(fast, run, 0)
                else:
                    print("    Switch input %d OFF" % (i + 1))
                v, t = watch(fast, lambda x: x == 0, 2.0 if lb else 60)
                run.check(t is not None, "input %d off: register 17 back to 0" % (i + 1),
                          "still on: " + bitlist(v or 0))

        # ---- 2: filter ----
        if "pulse" not in skip:
            p = args.pulse_input
            bit = p - 1
            run.section("2. Pulses on input %d, filter %d ms (the chip uses 10 ms)" % (p, FILTER_MS))
            for ms, should in ((5, False), (12, True)):
                c = PulseCounter(bit)
                if lb:
                    sent = 0
                    for _ in range(args.pulses):
                        t_on = time.perf_counter()
                        drive(fast, run, 1 << bit)
                        while time.perf_counter() - t_on < ms / 1000:
                            pass
                        drive(fast, run, 0)
                        sent += 1
                        t_end = time.perf_counter() + 0.06     # the filter delays by 10 ms; watch 60 ms
                        c.last_t = None                        # time between reads, within this window only
                        while time.perf_counter() < t_end:
                            v = fast.read(R_INPUTS)
                            if v is not None:
                                c(v)
                    c(0)
                else:
                    print("    Send %s pulses of %d ms to input %d now, at least 100 ms apart."
                          % ("at least 5" if not should else "10", ms, p))
                    countdown(fast, 20 if should else 15, c, stop_after=10 if should else None)
                    c(0)
                    sent = ask_int("How many %d ms pulses did you send?" % ms, 10 if should else 5)
                seen = len(c.widths)
                if should:
                    run.check(seen == sent and sent > 0, "%d ms pulses: all %d seen" % (ms, sent),
                              "%d seen of %d sent" % (seen, sent))
                    if c.widths:
                        run.out("  info  visible for %.1f to %.1f ms (reads up to %.1f ms apart)" % (
                            min(c.widths), max(c.widths), c.max_gap * 1000))
                    if seen < sent and not fast_enough:
                        run.out("  info  polling was slow: missed pulses may be the PC, not the board")
                else:
                    run.check(seen == 0 and sent > 0, "%d ms pulses: none of %d seen" % (ms, sent),
                              "%d seen of %d sent" % (seen, sent) if sent else "none sent")

        # ---- 3: wire break ----
        if "wb" not in skip:
            w, o = args.wb_input, args.other_input
            run.section("3. Wire break: input %d with its check on, input %d with it off" % (w, o))
            if node.writen(R_HOLD_DS, [hold, FILTER_MS, 1 << (w - 1), ol]).exc is not None:
                run.check(False, "WB_ENABLE written")
                return False
            print("    Input %d: wire-break resistor fitted at the sensor end, input off." % w)
            print("    Cut (disconnect) input %d's wire now." % w)
            v, t = wait_reg(node, R_WIREBREAK, lambda x: x != 0, 60)
            run.check(v == 1 << (w - 1), "input %d wire cut: WIREBREAK shows only input %d" % (w, w),
                      "shows %s" % bitlist(v or 0) if t is not None else "nothing within 60 s")
            print("    Reconnect input %d's wire." % w)
            v, t = wait_reg(node, R_WIREBREAK, lambda x: x == 0, 60)
            run.check(t is not None, "input %d reconnected: WIREBREAK back to 0" % w, "still " + bitlist(v or 0))
            print("    Now cut input %d's wire (its check is off). Watching 10 s." % o)
            v, t = wait_reg(node, R_WIREBREAK, lambda x: x != 0, 10)
            run.check(t is None, "input %d wire cut with its check off: WIREBREAK stays 0" % o,
                      "shows " + bitlist(v or 0))
            print("    Reconnect input %d's wire." % o)
    except RuntimeError as e:
        run.out("\n%s" % e)
        return False
    return True


def wait_reg(node, reg, until, timeout):
    t0 = time.monotonic()
    v = None
    while time.monotonic() - t0 < timeout:
        r = node.read(reg, 1)
        if r.ok:
            v = r.regs()[0]
            if until(v):
                return v, time.monotonic() - t0
        time.sleep(0.05)
    return v, None


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
