#!/usr/bin/env python3
import os
import sys
import subprocess
import glob

def main():
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    build_dir = os.path.join(project_root, ".pio", "build", "m5stickc-plus")
    
    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = os.path.join(build_dir, "firmware.bin")
    output = os.path.join(project_root, "merged-firmware.bin")
    
    # Locate boot_app0.bin in platformio packages
    home = os.path.expanduser("~")
    boot_app0_pattern = os.path.join(home, ".platformio", "packages", "framework-arduinoespressif32*", "tools", "partitions", "boot_app0.bin")
    boot_app0_candidates = glob.glob(boot_app0_pattern) + glob.glob(os.path.join(home, ".platformio", "packages", "framework-arduinoespressif32", "tools", "partitions", "boot_app0.bin"))
    
    boot_app0 = None
    for candidate in boot_app0_candidates:
        if os.path.exists(candidate):
            boot_app0 = candidate
            break
            
    if not boot_app0:
        print("Error: Could not locate boot_app0.bin in PlatformIO packages.")
        sys.exit(1)
        
    # Locate esptool.py in platformio packages
    esptool_pattern = os.path.join(home, ".platformio", "packages", "tool-esptoolpy*", "esptool.py")
    esptool_candidates = glob.glob(esptool_pattern) + glob.glob(os.path.join(home, ".platformio", "packages", "tool-esptoolpy", "esptool.py"))
    
    esptool = None
    for candidate in esptool_candidates:
        if os.path.exists(candidate):
            esptool = candidate
            break
            
    if not esptool:
        print("Error: Could not locate esptool.py in PlatformIO packages.")
        sys.exit(1)
        
    # Check if build files exist
    for f in [bootloader, partitions, firmware]:
        if not os.path.exists(f):
            print(f"Error: Required build file missing: {f}")
            print("Please run compilation first: python3 -m platformio run --environment m5stickc-plus")
            sys.exit(1)
            
    print("Found all required binaries. Merging...")
    cmd = [
        sys.executable,
        esptool,
        "--chip", "esp32",
        "merge_bin",
        "-o", output,
        "0x1000", bootloader,
        "0x8000", partitions,
        "0xe000", boot_app0,
        "0x10000", firmware
    ]
    
    try:
        subprocess.run(cmd, check=True)
        print(f"Success! Merged binary created at: {output}")
    except subprocess.CalledProcessError as e:
        print("Error: Failed to merge binaries.")
        sys.exit(e.returncode)

if __name__ == "__main__":
    main()
