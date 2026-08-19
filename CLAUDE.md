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
- **`docs/bench-test.md`** — the two-board bring-up procedure and its pass
  criteria.

Keep them current. `TODO.md` drifted badly once and claimed ESP-NOW was commented
out long after it had been written; a later version claimed a constant had been
renamed when the code still used the old name. Both were caught by reading the
code, not the docs — assume the code is the truth and fix the doc.

## Working on the firmware

- Pure logic belongs in `esp32-code/lib/`, where it is tested on the host
  (`pio test -e native`). Code that touches I2S, the radio or A2DP stays in
  `main.cpp`. Don't widen that split into a general refactor.
- New tuneable constants go in `include/config.h`, never inline. Per-node values
  go in `platformio.ini`.
- There are two builds for three boards, on purpose. If a board needs different
  *behaviour*, detect it at runtime — don't add a build config.
