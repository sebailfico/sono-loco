"""
Bake the git version into the firmware.

Run as a PlatformIO pre-build script (see `extra_scripts` in platformio.ini). It
defines FW_VERSION for every build, so a running board can say exactly which
commit produced it, and a bench log can be tied back to a tree state.

Why a script rather than a constant in config.h: a hand-maintained version is
wrong the moment someone forgets to bump it, and the value that actually matters
here -- "is this board running the code I am looking at?" -- is not something a
human can keep in step by hand. This one cannot drift, because nobody types it.

The string is `git describe --tags --always --dirty`, e.g.

    v0.1.0                 exactly the tagged commit, clean tree
    v0.1.0-3-gabc1234      3 commits past v0.1.0, at abc1234
    v0.1.0-3-gabc1234*     ...with uncommitted changes in the tree

`--dirty=*` rather than the default `-dirty` keeps the string short enough for
the serial banner, and the marker is what matters: a dirty build is one whose
sha does NOT describe what is on the board, so any measurement taken from it is
unreproducible. The bench harness treats it as a warning for that reason.

Falls back to "unknown" when git is missing or this is not a checkout (a release
tarball, say). A build must never fail for want of a version string.
"""

import subprocess

Import("env")


def git_describe():
    try:
        out = subprocess.check_output(
            ["git", "describe", "--tags", "--always", "--dirty=*"],
            cwd=env.subst("$PROJECT_DIR"),
            stderr=subprocess.DEVNULL,
        )
        return out.decode().strip() or "unknown"
    except Exception:
        return "unknown"


version = git_describe()
print(f"SonoLoco firmware version: {version}")

# CPPDEFINES with a tuple gives -DFW_VERSION=\"...\" with the quoting handled by
# SCons. Do not hand-roll the escaping; it differs between the shells PlatformIO
# may spawn.
env.Append(CPPDEFINES=[("FW_VERSION", env.StringifyMacro(version))])
