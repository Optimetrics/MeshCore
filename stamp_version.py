#!/usr/bin/python3

# PlatformIO pre-script: stamps every build with the git commit and today's date, the way
# build.sh does for releases, so 'ver' (and the version sent to clients) says exactly what
# is running. Does nothing when FIRMWARE_VERSION is already supplied in the build flags.
#
# Result: "v1.18.0-1a2b3c4d", with a trailing "*" when tracked files had uncommitted changes.
# The base comes from the 'custom_fw_version' option in the env (default below).

import datetime
import os
import subprocess

Import("env")

DEFAULT_BASE = "v1.18.0"


def git(*args):
    try:
        return subprocess.check_output(("git",) + args, cwd=env["PROJECT_DIR"], text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except Exception:
        return ""


flags = " ".join(env.GetProjectOption("build_flags", [])) + " " + os.environ.get("PLATFORMIO_BUILD_FLAGS", "")
if "FIRMWARE_VERSION" not in flags:
    commit = git("rev-parse", "--short=8", "HEAD") or "nogit"
    dirty = "*" if git("status", "--porcelain", "--untracked-files=no") else ""
    version = "%s-%s%s" % (env.GetProjectOption("custom_fw_version", DEFAULT_BASE), commit, dirty)
    built = datetime.date.today().strftime("%d %b %Y")
    env.Append(CPPDEFINES=[("FIRMWARE_VERSION", env.StringifyMacro(version)),
                           ("FIRMWARE_BUILD_DATE", env.StringifyMacro(built))])
    print("Firmware version stamp: %s (Build: %s)" % (version, built))
