#!/usr/bin/env python3
"""wait_console.py -- poll for a FreeWili OG CPU's USB CDC port and attach a
terminal to it the instant it enumerates, so nothing printed in the first
seconds after a reset is missed waiting for a human to notice the port and
type a command by hand.

Examples:
  python wait_console.py --cpu main                    # launch PuTTY (default)
  python wait_console.py --cpu display --term teraterm
  python wait_console.py --cpu main --term print        # print straight to this console, no GUI app
  python wait_console.py --cpu main --fresh             # wait for it to drop, then reappear
  python wait_console.py --pid 2054                     # match by raw PID instead of --cpu

Identification matches fw.py's own scheme: Intrepid's VID (0x093C) plus the
per-CPU PID (main 0x2054, display 0x2055) that fwog_usb_ids() sets. A board
still running the SDK's stock identity, or a factory image that predates
this BSP, won't match -- pass --vid/--pid directly if you need to.

Needs pyserial (`pip install pyserial`), already a fw.py dependency.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import time

ICS_VID = 0x093C
CPU_PID = {"main": 0x2054, "display": 0x2055}


def _matching_ports(vid, pid):
    from serial.tools import list_ports
    return {p.device for p in list_ports.comports() if p.vid == vid and p.pid == pid}


def wait_for_port(vid, pid, fresh, poll_s, timeout_s):
    """Block until a port matching vid:pid is available, then return it (or
    None on timeout).

    `fresh`: if a match already exists when called, wait for it to disappear
    first (confirming a reset is actually underway) before waiting for one to
    reappear. Without this, a board that never resets (already booted before
    this script started) is indistinguishable from one that's mid-reset, and
    Windows often reassigns the SAME COM number to the same physical port, so
    a plain "is it there" check can't always tell old from new -- only a
    disappear-then-reappear transition can."""
    deadline = None if timeout_s is None else time.time() + timeout_s

    def remaining():
        return None if deadline is None else max(0.0, deadline - time.time())

    if fresh and _matching_ports(vid, pid):
        print(f"waiting for the current VID {vid:04X}:PID {pid:04X} port to "
              "drop (reset in progress) ...", file=sys.stderr)
        while _matching_ports(vid, pid):
            if deadline is not None and time.time() > deadline:
                return None
            time.sleep(poll_s)

    print(f"waiting for VID {vid:04X}:PID {pid:04X} to enumerate ...",
          file=sys.stderr)
    while True:
        found = _matching_ports(vid, pid)
        if found:
            return sorted(found)[0]
        if deadline is not None and time.time() > deadline:
            return None
        time.sleep(poll_s)


def _find_exe(names, extra_dirs):
    for name in names:
        found = shutil.which(name)
        if found:
            return found
    for d in extra_dirs:
        for name in names:
            p = pathlib.Path(d) / name
            if p.exists():
                return str(p)
    return None


def launch_putty(port, baud):
    exe = _find_exe(["putty.exe", "putty"],
                    [r"C:\Program Files\PuTTY", r"C:\Program Files (x86)\PuTTY"])
    if not exe:
        print("putty.exe not found on PATH or in the usual install dirs -- "
              "pass its path with --exe, or use --term print instead",
              file=sys.stderr)
        return 1
    print(f"launching PuTTY on {port} ...", file=sys.stderr)
    subprocess.Popen([exe, "-serial", port, "-sercfg", f"{baud},8,n,1,N"])
    return 0


def launch_teraterm(port, baud):
    exe = _find_exe(["ttermpro.exe"],
                    [r"C:\Program Files (x86)\teraterm", r"C:\Program Files\teraterm"])
    if not exe:
        print("ttermpro.exe not found on PATH or in the usual install dirs -- "
              "pass its path with --exe, or use --term print instead",
              file=sys.stderr)
        return 1
    num = "".join(ch for ch in port if ch.isdigit())
    print(f"launching Tera Term on {port} ...", file=sys.stderr)
    subprocess.Popen([exe, f"/C={num}", f"/BAUD={baud}"])
    return 0


def print_console(port, baud):
    import serial
    # The port can report present via list_ports slightly before Windows
    # finishes attaching it -- a handful of quick retries covers that race
    # without a human needing to re-run the command.
    ser = None
    for _ in range(20):
        try:
            ser = serial.Serial(port, baud, timeout=0.2)
            break
        except serial.SerialException:
            time.sleep(0.1)
    if ser is None:
        print(f"could not open {port} after it enumerated", file=sys.stderr)
        return 1
    with ser:
        print(f"connected to {port} -- Ctrl-C to exit", file=sys.stderr)
        try:
            while True:
                data = ser.read(4096)
                if data:
                    sys.stdout.write(data.decode("utf-8", "replace"))
                    sys.stdout.flush()
        except KeyboardInterrupt:
            pass
    return 0


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--cpu", choices=sorted(CPU_PID), help="which FreeWili CPU to wait for")
    g.add_argument("--pid", type=lambda s: int(s, 0), help="match this raw USB PID instead")
    ap.add_argument("--vid", type=lambda s: int(s, 0), default=ICS_VID,
                    help=f"override VID (default: Intrepid, 0x{ICS_VID:04X})")
    ap.add_argument("--term", choices=["putty", "teraterm", "print"], default="putty",
                    help="what to do once the port appears (default: putty)")
    ap.add_argument("--exe", help="explicit path to putty.exe/ttermpro.exe")
    ap.add_argument("--baud", type=int, default=115200,
                    help="ignored by USB CDC itself, but some terminals need a value")
    ap.add_argument("--fresh", action="store_true",
                    help="if the port already exists, wait for it to drop before "
                         "waiting for it to reappear -- use this when the board "
                         "is already running and you're about to reset it")
    ap.add_argument("--timeout", type=float, default=None,
                    help="give up after this many seconds (default: wait forever)")
    ap.add_argument("--poll", type=float, default=0.1,
                    help="how often to check for the port, in seconds (default: 0.1)")
    a = ap.parse_args()

    pid = a.pid if a.pid is not None else CPU_PID[a.cpu]
    port = wait_for_port(a.vid, pid, a.fresh, a.poll, a.timeout)
    if not port:
        print(f"timed out waiting for VID {a.vid:04X}:PID {pid:04X}", file=sys.stderr)
        return 1
    print(f"found {port}", file=sys.stderr)

    if a.exe:
        # An explicit --exe still needs to know which app it is to build the
        # right argument list; infer from the chosen --term rather than
        # adding a second flag for it.
        if a.term == "teraterm":
            num = "".join(ch for ch in port if ch.isdigit())
            subprocess.Popen([a.exe, f"/C={num}", f"/BAUD={a.baud}"])
        else:
            subprocess.Popen([a.exe, "-serial", port, "-sercfg", f"{a.baud},8,n,1,N"])
        return 0

    if a.term == "putty":
        return launch_putty(port, a.baud)
    if a.term == "teraterm":
        return launch_teraterm(port, a.baud)
    return print_console(port, a.baud)


if __name__ == "__main__":
    sys.exit(main())
