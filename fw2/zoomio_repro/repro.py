"""Run a ZoomIO program on a FreeWili 2 from the PC and time GPIO 25.

No display app involved: the board runs stock firmware on both CPUs, and this
script talks to MAIN over USB with the OneWili Python package. It

  1. switches power zone 6 (FPGA) on -- MAIN refuses header GPIO and ZoomIO
     with "EPOWERZONE 6 FPGA" otherwise, and nothing raises it by default;
  2. stops any running ZoomIO program and drives GPIO 25 low;
  3. uploads the .zio to /appdata/zio_repro/ and asks MAIN to run it;
  4. polls GPIO 25 and prints each edge and the mean time per level.

The readback is MAIN's internal pin state, so it shows whether core1 is
toggling the pin; it does not need VIO or anything wired to the header.

Usage (from a checkout with wilibsp/ beside this folder, or pass --onewili):
    python repro.py blink_ms.zio
    python repro.py blink_cyc.zio --seconds 30

Requires: pip install pyfwfinder pyserial result
"""
import argparse
import pathlib
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_ONEWILI = HERE.parent.parent / "wilibsp" / "libs" / "onewili" / "python"
DEV_DIR = "/appdata/zio_repro"


def check(label, result, fatal=True):
    ok = result.is_ok()
    print(f"{label}: {result}")
    if not ok and fatal:
        sys.exit(f"{label} failed; stopping")
    return ok


def gpio_bits(dev):
    r = dev.io.gpio.read_all()
    if not r.is_ok():
        return None
    v = r.unwrap()
    if isinstance(v, (tuple, list)):
        v = v[0]
    return int(v, 16) if isinstance(v, str) else int(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("zio", type=pathlib.Path)
    ap.add_argument("--seconds", type=float, default=30.0, help="how long to watch the pin")
    ap.add_argument("--pin", type=int, default=25)
    ap.add_argument("--expect-ms", type=float, default=250.0, help="nominal time per level")
    ap.add_argument("--poll-ms", type=float, default=5.0,
                    help="sleep between GPIO reads; vary it to rule out aliasing")
    ap.add_argument("--leave-running", action="store_true", help="do not stop ZoomIO at the end")
    ap.add_argument("--onewili", type=pathlib.Path, default=DEFAULT_ONEWILI,
                    help="path to the OneWili python package directory")
    ap.add_argument("--serial", help="FreeWili serial number when several are connected")
    a = ap.parse_args()

    sys.path.insert(0, str(a.onewili))
    import onewili

    dev = onewili.connect(a.serial)
    zoom = dev.scripting.zoom_io

    check("zone 6 on", dev.hardware.power_management.set_zone(6, 1))
    time.sleep(1.5)
    check("stop_zio", zoom.stop_zio(), fatal=False)
    check(f"gpio {a.pin} low", dev.io.gpio.set_io_low(a.pin))

    dev.hardware.file_system.create_directory("/appdata")   # fails if it exists
    dev.hardware.file_system.create_directory(DEV_DIR)
    dev_path = f"{DEV_DIR}/{a.zio.name}"
    dev.files.put_file(a.zio, dev_path)
    print(f"uploaded {a.zio.name} -> {dev_path}")

    t0 = time.monotonic()
    ran = zoom.run_zio(dev_path)
    print(f"run_zio: {ran} after {time.monotonic() - t0:.2f} s")
    if not ran.is_ok():
        # The reply that never comes is itself the finding for blink_cyc.zio;
        # check whether MAIN still answers anything at all.
        alive = gpio_bits(dev) is not None
        print(f"MAIN answers a GPIO read afterwards: {alive}")
        sys.exit(1)

    print(f"watching GPIO {a.pin} for {a.seconds:.0f} s ...")
    last = None
    edges = []
    errors = 0
    reads = 0
    start = time.monotonic()
    end = start + a.seconds
    while time.monotonic() < end:
        bits = gpio_bits(dev)
        now = time.monotonic()
        reads += 1
        if bits is None:
            errors += 1
            continue
        level = (bits >> a.pin) & 1
        if last is not None and level != last:
            since = f"{(now - edges[-1]) * 1000:.0f} ms since last" if edges else "first"
            print(f"  edge {len(edges)} -> {level}  ({since})")
            edges.append(now)
        last = level
        time.sleep(a.poll_ms / 1000)
    print(f"sampled every {(time.monotonic() - start) / reads * 1000:.1f} ms on average "
          f"({reads} reads)")

    if len(edges) >= 2:
        per_level = (edges[-1] - edges[0]) / (len(edges) - 1) * 1000
        print(f"mean per level: {per_level:.1f} ms over {len(edges)} edges "
              f"(expected {a.expect_ms:.0f} ms, ratio {per_level / a.expect_ms:.2f}x)")
    else:
        print(f"only {len(edges)} edge(s) seen; level per edge exceeds the window or the pin is not toggling")
    print(f"read errors: {errors}")

    if not a.leave_running:
        check("stop_zio", zoom.stop_zio(), fatal=False)


if __name__ == "__main__":
    main()
