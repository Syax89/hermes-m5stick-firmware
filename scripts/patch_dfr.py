#!/usr/bin/env python3
"""Auto-patch DFRobot_GP8XXX for ESP32 Arduino Core v3+ compatibility.

The library calls analogWriteResolution(pin, bits) with two args,
but ESP32 Arduino Core v3+ changed the API to single-arg (bits only).
This script runs after 'pio pkg install' to fix the file in-place.
"""
Import("env")
import os

lib_path = os.path.join(
    env.subst("$PROJECT_LIBDEPS_DIR"),
    env.subst("$PIOENV"),
    "DFRobot_GP8XXX",
    "DFRobot_GP8XXX.cpp",
)

if os.path.isfile(lib_path):
    with open(lib_path, "r") as f:
        content = f.read()

    old = "analogWriteResolution(_pin0,10);"
    new = "analogWriteResolution(10);"

    if old in content:
        content = content.replace(old, new)
        with open(lib_path, "w") as f:
            f.write(content)
        print("[patch] DFRobot_GP8XXX.cpp fixed for ESP32 Arduino Core v3+")
    else:
        print("[patch] DFRobot_GP8XXX.cpp already patched or not needed")
