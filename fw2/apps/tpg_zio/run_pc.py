"""Run one generated TPG program on a FreeWili 2 from the PC.

Bench helper for gen_tpg_zio.py output: switches power zone 6 on, drives
every pin the program declares with setiodir low through the stock firmware
and sets its header level shifter to output, uploads the program to
/appdata/tpg_zio_pc/, and runs it. With --marker it then times the field-marker
pin (build the programs with gen_tpg_zio.py --marker) and compares the field
period with the nominal 262 x 9534 cycles at 150 MHz.

    python gen_tpg_zio.py --out-dir out --marker 25
    python run_pc.py out/tpg_color_bars.zio --marker 25
"""
import argparse
import pathlib
import re
import sys
import time

HERE = pathlib.Path(__file__).resolve().parent
DEFAULT_ONEWILI = HERE.parents[2] / "wilibsp" / "libs" / "onewili" / "python"
DEV_DIR = "/appdata/tpg_zio_pc"   # not the app's folder: same file names, other builds
FIELD_MS = 262 * 9534 / 150_000.0     # 16.652712 ms

# Header GPIO -> its item in MAIN's IO Directions menu (i\g\a). The items are
# documented as toggles, but take an optional value: 1 = out, 0 = in.
DIR_KEY = {8: "g", 9: "e", 10: "f", 11: "m", 12: "a", 13: "c", 14: "j", 15: "i",
           25: "k", 26: "b", 27: "l"}


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("zio", type=pathlib.Path)
    ap.add_argument("--marker", type=int, help="field-marker GPIO to time")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--leave-running", action="store_true")
    ap.add_argument("--onewili", type=pathlib.Path, default=DEFAULT_ONEWILI)
    a = ap.parse_args()

    sys.path.insert(0, str(a.onewili))
    import onewili
    dev = onewili.connect()
    zoom = dev.scripting.zoom_io

    def must(label, r):
        if not r.is_ok():
            sys.exit(f"{label}: {r}")

    must("zone 6 on", dev.hardware.power_management.set_zone(6, 1))
    time.sleep(1.5)
    zoom.stop_zio()
    pins = [int(p) for p in re.findall(r"^setiodir (\d+), 1", a.zio.read_text(), re.M)]
    for p in pins:
        must(f"gpio {p} low", dev.io.gpio.set_io_low(p))
        must(f"gpio {p} header out", dev.io.gpio.io_direction_settings._call(DIR_KEY[p], ["1"], []))
        # ~0.3 s after that reply MAIN prints an "updateHardwareFromSettings()"
        # message with no newline, so the next reply would share its line, and
        # onewili's transport only sees frames at the start of a line. Wait
        # for it, then end the line with a throwaway `?` and discard both.
        time.sleep(0.6)
        dev._transport.send("?")
        time.sleep(0.3)
        dev._transport.flush_queues()
    print(f"pins low, header outputs: {pins}")

    dev.hardware.file_system.create_directory("/appdata")
    dev.hardware.file_system.create_directory(DEV_DIR)
    path = f"{DEV_DIR}/{a.zio.name}"
    dev.files.put_file(a.zio, path)
    r = zoom.run_zio(path)
    print(f"run {a.zio.name} ({a.zio.stat().st_size} bytes): {r}")
    if not r.is_ok():
        sys.exit(1)

    if a.marker is not None:
        # perf_counter, not monotonic: on Windows monotonic ticks every ~15.6 ms,
        # which is about one field and swamps the measurement.
        edges, last, end = [], None, time.perf_counter() + a.seconds
        while time.perf_counter() < end:
            t0 = time.perf_counter()
            r = dev.io.gpio.read_all()
            if not r.is_ok():
                continue
            v = r.unwrap()
            v = int(v, 16) if isinstance(v, str) else int(v[0] if isinstance(v, (tuple, list)) else v)
            level = (v >> a.marker) & 1
            if last is not None and level != last:
                edges.append((t0 + time.perf_counter()) / 2)
            last = level
        gaps = [b - a_ for a_, b in zip(edges, edges[1:])]
        if len(edges) < 3:
            print(f"marker: only {len(edges)} edges -- program not toggling GPIO {a.marker}?")
        elif max(gaps) * 1000 > 1.5 * FIELD_MS:
            # The marker toggles every field, so a long gap hides an even number
            # of edges and the field count is unknowable.
            print(f"marker: a poll stalled {max(gaps) * 1000:.1f} ms and edges were missed; "
                  f"run again")
        else:
            fields = len(gaps)
            per = (edges[-1] - edges[0]) * 1000 / fields
            print(f"field period {per:.4f} ms over {fields} fields "
                  f"(nominal {FIELD_MS:.4f} ms, error {(per / FIELD_MS - 1) * 1e6:+.0f} ppm); "
                  f"line rate {262 / per:.3f} kHz")

    if not a.leave_running:
        zoom.stop_zio()


if __name__ == "__main__":
    main()
