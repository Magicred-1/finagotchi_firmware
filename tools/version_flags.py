# version_flags.py — PlatformIO pre-script: inject the git short hash as
# -DFW_BUILD=\"<hash>\" (src/version.h falls back to "dev" without it).
# Runs for every env (declared once in [env:esp32-s3], inherited via extends).

import subprocess

Import("env")

try:
    rev = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        cwd=env["PROJECT_DIR"],
        stderr=subprocess.DEVNULL,
    ).decode().strip()
except Exception:
    rev = "unknown"

env.Append(BUILD_FLAGS=['-DFW_BUILD=\\"%s\\"' % rev])
print("version_flags: FW_BUILD=%s" % rev)
