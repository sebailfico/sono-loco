# CLAUDE.md — SonoLoco

Everything is in the docs, kept in one place so they stay readable for humans too.
Each file owns one thing; that separation is the point, so don't duplicate content
between them.

- **`README.md`** — what the project is, the state machine, hardware and wiring,
  the build/flash/test commands, the code layout, and the list of gotchas that
  have already bitten this code. Read it before changing anything.
- **`TODO.md`** — what is still open. Only open items.
- **`CHANGELOG.md`** — what has been done, newest first. Completed work moves
  here out of `TODO.md`.
- **`docs/decisions.md`** — why the architecture is what it is, and for each
  decision, what would change it. Read this before proposing a different
  approach; the common alternatives have already been considered and the
  reasoning is recorded. If a decision genuinely changes, edit the entry rather
  than deleting it.
- **`docs/bench-test.md`** — how to test on real hardware: the automated
  multi-board harness first, then the manual walkthrough for the Bluetooth path,
  which cannot be automated.

Keep them current. `TODO.md` drifted badly once and claimed ESP-NOW was commented
out long after it had been written; a later version claimed a constant had been
renamed when the code still used the old name. Both were caught by reading the
code, not the docs — assume the code is the truth and fix the doc.

## Working on the firmware

- Pure logic belongs in `esp32-code/lib/`, where it is tested on the host
  (`pio test -e native`, or `pio test -e esp32dev` on a board — there is no host
  compiler on this machine yet). Code that touches I2S, the radio or A2DP stays
  in `main.cpp`. Don't widen that split into a general refactor.
- New tuneable constants go in `include/config.h`, never inline. Per-node values
  go in `platformio.ini`.
- The firmware version is `git describe`, injected by `scripts/version.py` at
  build time. Never add a version constant to bump by hand. Bench results and
  `CHANGELOG.md` entries carry that version — a measurement taken from a dirty
  tree (trailing `*`) is not reproducible, so commit before measuring anything
  worth recording. See D10.
- There are two builds for three board types, on purpose. If a board needs
  different *behaviour*, detect it at runtime — don't add a build config.
- **Anything touching the client audio path must be re-measured, not reasoned
  about.** Run `./tools/bench-mesh.ps1 -Flash -Duration 600` and compare against
  the recorded baseline in `CHANGELOG.md`: zero lost/ovf/und/dup/rsy, source at
  220.5 pkt/s, drift about -30 ppm. The worst bug found so far — a permanent
  24/s underrun — was invisible in the code and obvious in that output.
- Drift figures need long runs. At 45 s the measurement noise exceeds the effect;
  600 s is the shortest run worth quoting.
