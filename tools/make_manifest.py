#!/usr/bin/env python3
# make_manifest.py — generate the API repo's data/firmware.json snippet for
# a release: per-env {version, url, sha256, size, notes} from the built
# binaries. Used by .github/workflows/release.yml.
#
#   python3 tools/make_manifest.py <version> <repo> <tag> <bin-dir> <out.json>
#
# The snippet's url points at the API's own /firmware/files/ route. The API
# relays the corresponding GitHub Release asset because devices pin the ISRG
# Root X1 CA and cannot use GitHub's certificate chain. Publishing OTA means
# pasting this snippet into data/firmware.json and redeploying the API. The
# `repo` arg is kept for CLI stability with the workflow; it no longer feeds
# the URL. The sim env never OTA-updates and gets no manifest entry.

import hashlib
import json
import os
import sys

ENVS = ("esp32-s3", "pcb-v1")
FILES_BASE_URL = "https://api.finagotchi.app/firmware/files"


def main():
    version, _repo, tag, bin_dir, out = sys.argv[1:6]
    snippet = {}
    for env in ENVS:
        name = f"finagotchi-{env}-{version}.bin"
        with open(os.path.join(bin_dir, name), "rb") as f:
            data = f.read()
        snippet[env] = {
            "version": version,
            "url": f"{FILES_BASE_URL}/{env}/{version}/{name}",
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
            "notes": f"Firmware {version} ({tag})",
        }
    with open(out, "w") as f:
        json.dump(snippet, f, indent=2)
    print(json.dumps(snippet, indent=2))


if __name__ == "__main__":
    main()
