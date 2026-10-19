#!/usr/bin/env python3
import os
import sys
import subprocess
import glob

def find_boot_app0():
    candidates = glob.glob(os.path.expanduser("~/.platformio/packages/framework-arduinoespressif32*/tools/partitions/boot_app0.bin"))
    if candidates:
        return candidates[0]
    return None

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    build_dir = os.path.join(script_dir, ".pio", "build", "m5stack-stamps3")

    bootloader = os.path.join(build_dir, "bootloader.bin")
    partitions = os.path.join(build_dir, "partitions.bin")
    firmware = os.path.join(build_dir, "firmware.bin")
    boot_app0 = find_boot_app0()

    if not os.path.exists(bootloader) or not os.path.exists(partitions) or not os.path.exists(firmware):
        print("Error: Missing build artifacts. Run 'pio run' first.")
        sys.exit(1)

    if not boot_app0 or not os.path.exists(boot_app0):
        print("Error: boot_app0.bin not found in ~/.platformio packages.")
        sys.exit(1)

    output_bin = os.path.join(project_root, "GeminiPuter.bin")
    m5burner_bin = os.path.join(project_root, "GeminiPuter-v2.0.1-merged-M5Burner.bin")

    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32s3",
        "merge-bin",
        "-o", output_bin,
        "--flash-mode", "dio",
        "--flash-freq", "80m",
        "--flash-size", "8MB",
        "0x0000", bootloader,
        "0x8000", partitions,
        "0xe000", boot_app0,
        "0x10000", firmware
    ]

    print("Merging binaries into a single flashable image (0x0 offset)...")
    res = subprocess.run(cmd)
    if res.returncode == 0:
        import shutil
        shutil.copyfile(output_bin, m5burner_bin)
        print(f"Success! Created:\n  -> {output_bin}\n  -> {m5burner_bin}")
    else:
        print("Merge failed.")
        sys.exit(res.returncode)

if __name__ == "__main__":
    main()
