#!/usr/bin/env python3
"""set_address.py - read or set a CEMS I/O board's Modbus address over its USB-C port (FW-60).

The address lives in the board's F-RAM, not in the controller's flash, so firmware updates
(even with a full chip erase) never change it. A new board has no address (0) and stays silent
on RS-485 with its comms LED steady on until one is set here.

  python set_address.py COM7                 show the board and its address
  python set_address.py COM7 --address 3     set it (1..247)
  python set_address.py COM7 --clear         take the board off the bus (address 0)

Connect to the board's USB-C port only (it answers at the service address 247 whatever its own
address). The board refuses the change (exception 04) while the main board is controlling its
outputs: disconnect the RS-485 cable, or wait for the board's hold time to run out, first.

Needs Python 3.8+ and pyserial (pip install pyserial). Exit code 0 when done, 1 on a refusal or a
failed read-back, 2 when the board cannot be reached.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from at02 import Link, Node, USB_ADDRESS, MAGIC  # noqa: E402

R_NODE_ADDR = 38
EXC_TEXT = {
    2: "the board refused it as read-only: is this the RS-485 port rather than USB?",
    3: "out of range (0..247)",
    4: "the main board is in control of this board, or the address could not be saved to the F-RAM. "
       "Disconnect the RS-485 cable (or wait for the hold time to run out) and try again; if it still "
       "fails, the board's F-RAM is not answering",
}


def main():
    ap = argparse.ArgumentParser(description="Read or set a CEMS I/O board's address over USB.")
    ap.add_argument("port", help="the board's USB serial port: COM7, /dev/ttyACM0 ...")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("-a", "--address", type=int, help="new address, 1..247")
    g.add_argument("--clear", action="store_true", help="set address 0: the board stays off the RS-485 bus")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    new = 0 if args.clear else args.address
    if new is not None and not 0 <= new <= 247:
        ap.error("--address must be 1..247")

    link = Link(args.port, 115200, 0.3, 0.01, args.verbose)
    node = Node(link, USB_ADDRESS)
    try:
        ident = node.regs(64, 15)
        addr = node.regs(R_NODE_ADDR, 1)[0]
    except IOError as e:
        print("Cannot reach the board: %s" % e)
        print("Is this the board's USB-C port, and is the board powered?")
        return 2
    if ident[0] != MAGIC:
        print("This does not look like a CEMS I/O board (MAGIC 0x%04X)." % ident[0])
        return 2
    uid = "".join("%04X" % u for u in reversed(ident[5:11]))
    fw = ident[3]
    print("CEMS I/O board  UID %s  hardware rev %d  firmware %d.%d build %d" % (
        uid, ident[2], fw >> 8, fw & 0xFF, ident[4]))
    print("Address: %s" % (addr if addr else "not set (silent on RS-485)"))
    if new is None:
        return 0
    if new == addr:
        print("Already %d: nothing to do." % new)
        return 0

    r = node.write1(R_NODE_ADDR, new)
    if not r.ok:
        why = EXC_TEXT.get(r.exc, r.what()) if r.exc is not None else r.what()
        print("Not changed: %s" % why)
        return 1
    back = node.regs(R_NODE_ADDR, 1)[0]
    if back != new:
        print("Read back %d instead of %d: not changed." % (back, new))
        return 1
    print("Address set to %s and saved in the board's F-RAM." % (new if new else "0 (off the bus)"))
    if new:
        print("Enter %d as this board's address on the main board (I/O board settings)." % new)
    return 0


if __name__ == "__main__":
    sys.exit(main())
