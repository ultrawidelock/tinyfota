#!/usr/bin/env python3
"""Prove the sample's two flash maps agree, and the door-lock fragment's with them.

    python tests/tooling/partition_map_check.py      # with tinyfota installed

NCS reads samples/nrf52833/pm_static.yml and upstream Zephyr reads
samples/nrf52833/partitions.dtsi. A board flashed from one and updated from a
build on the other is only safe while every partition has the same address and
size in both, so each is read with the parser `tinyfota build --build-dir` uses
and compared. samples/door-lock-fragment/pm_static.yml is the copy an
integrator takes; it is compared against the sample's pm_static.yml.
"""

import sys
from pathlib import Path

from tinyfota.patch import dts_partition_field, pm_partition_field

SAMPLE = Path(__file__).resolve().parents[2] / "samples" / "nrf52833"
FRAGMENT = Path(__file__).resolve().parents[2] / "samples" / "door-lock-fragment"

# Partition Manager name -> devicetree label, for every partition in both maps.
PAIRS = {
    "mcuboot": "boot_partition",
    "mcuboot_primary": "slot0_partition",
    "patch_staging": "patch_staging",
    "settings_storage": "storage_partition",
}


def show(value):
    return "missing" if value is None else hex(value)


pm = (SAMPLE / "pm_static.yml").read_text()
dts = (SAMPLE / "partitions.dtsi").read_text()
fails = 0
for name, label in PAIRS.items():
    for field in ("address", "size"):
        a = pm_partition_field(pm, name, field)
        b = dts_partition_field(dts, label, field)
        ok = a is not None and a == b
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name} {field}: "
              f"pm_static.yml {show(a)}, partitions.dtsi {show(b)}")

frag = (FRAGMENT / "pm_static.yml").read_text()
for name in ("mcuboot", "mcuboot_pad", "app", "mcuboot_primary_app", "mcuboot_primary",
             "patch_staging", "settings_storage"):
    for field in ("address", "size"):
        a = pm_partition_field(pm, name, field)
        b = pm_partition_field(frag, name, field)
        ok = a is not None and a == b
        fails += not ok
        print(f"  {'ok  ' if ok else 'FAIL'} {name} {field}: "
              f"sample {show(a)}, door-lock fragment {show(b)}")

print(f"  partition maps: {'PASS' if not fails else 'FAIL'}")
sys.exit(1 if fails else 0)
