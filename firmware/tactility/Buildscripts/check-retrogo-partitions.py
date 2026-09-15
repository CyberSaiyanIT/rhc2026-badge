#!/usr/bin/env python3
"""Checks each Retro-Go app against the partition slot it is flashed into.

The Retro-Go build never sees the real table: rg_tool.py writes a dummy 3 MB partition for
esp-idf, so an app that outgrows its slot is only noticed when the truncated write fails to
boot. With --port the board's own table is read as well, since apps are flashed by label into
whatever the board reports rather than into the table kept here.
"""
import argparse
import csv
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
TABLE = ROOT / "firmware" / "tactility" / "partitions-16mb-no-sd-retrogo.csv"
RETRO_GO = ROOT / "firmware" / "retro-go"
DEFAULT_APPS = ["launcher", "retro-core", "prboom-go"]


def parse_size(text):
    text = text.strip()
    if text.lower().endswith("k"):
        return int(text[:-1], 0) * 1024
    if text.lower().endswith("m"):
        return int(text[:-1], 0) * 1024 * 1024
    return int(text, 0)


def read_sizes(path):
    sizes = {}
    with open(path) as handle:
        for row in csv.reader(handle):
            if not row or row[0].strip().startswith("#") or len(row) < 5:
                continue
            try:
                sizes[row[0].strip()] = parse_size(row[4])
            except ValueError:
                continue  # header or comment row that csv did not recognise as one
    return sizes


def read_device_table(port):
    """Dumps the table the board is actually running. Returns None when the tools are absent."""
    idf = os.environ.get("IDF_PATH")
    gen = Path(idf, "components", "partition_table", "gen_esp32part.py") if idf else None
    if not gen or not gen.is_file():
        print("WARNING: IDF_PATH is not set, skipping the on-device check", file=sys.stderr)
        return None
    with tempfile.TemporaryDirectory() as tmp:
        binary, table = Path(tmp, "partitions.bin"), Path(tmp, "device.csv")
        read = subprocess.run(
            [sys.executable, "-m", "esptool", "--chip", "esp32s3", "--port", port,
             "--before", "default_reset", "--after", "no_reset",
             "read_flash", "0x8000", "0x1000", str(binary)],
            capture_output=True,
        )
        if read.returncode != 0:
            sys.exit(f"ERROR: could not read the partition table from {port}")
        with open(table, "w") as handle:
            subprocess.run([sys.executable, str(gen), str(binary)], stdout=handle, check=True)
        return read_sizes(table)


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("apps", nargs="*", default=DEFAULT_APPS,
                        help=f"apps to check (default: {' '.join(DEFAULT_APPS)})")
    parser.add_argument("--port", help="also check the table on the board at this serial port")
    args = parser.parse_args()

    expected = read_sizes(TABLE)
    device = read_device_table(args.port) if args.port else None
    problems = []

    for app in args.apps:
        limit = expected.get(app)
        if limit is None:
            problems.append(f"{app}: no partition named '{app}' in {TABLE.name}")
            continue

        binary = RETRO_GO / app / "build" / (app + ".bin")
        if binary.is_file():
            used = binary.stat().st_size
            status = "OK" if used <= limit else "TOO LARGE"
            print(f"{app:<12} {used:>8} / {limit:>8} bytes ({used * 100.0 / limit:5.1f}%) {status}")
            if used > limit:
                problems.append(f"{app}: {used} bytes does not fit its {limit} byte slot")
        else:
            print(f"{app:<12} not built ({binary.relative_to(ROOT)})")

        if device is not None:
            if app not in device:
                problems.append(f"{app}: the board has no '{app}' partition")
            elif device[app] < limit:
                problems.append(
                    f"{app}: the board's slot is {device[app]} bytes, this table expects {limit}"
                )

    if problems:
        print()
        for problem in problems:
            print(f"ERROR: {problem}", file=sys.stderr)
        if device is not None:
            print("The board is running a Tactility built against a different partition table.",
                  file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()
