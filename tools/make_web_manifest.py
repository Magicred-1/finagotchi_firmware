#!/usr/bin/env python3

import json
import sys

OFFSETS = {
    "bootloader": 0x0,
    "partitions": 0x8000,
    "boot_app0": 0xE000,
    "firmware": 0x10000,
}


def main():
    version, repo, tag, env, out = sys.argv[1:6]
    board_name = "Finagotchi PCB v1" if env == "pcb-v1" else "Finagotchi ESP32-S3"
    parts = []
    for part, offset in OFFSETS.items():
        suffix = "" if part == "firmware" else f"-{part}"
        filename = f"finagotchi-{env}-{version}{suffix}.bin"
        parts.append({
            "path": f"https://github.com/{repo}/releases/download/{tag}/{filename}",
            "offset": offset,
        })
    manifest = {
        "name": board_name,
        "version": version,
        "new_install_prompt_erase": True,
        "new_install_improv_wait_time": 0,
        "builds": [{"chipFamily": "ESP32-S3", "parts": parts}],
    }
    with open(out, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
