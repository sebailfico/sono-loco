# SonoLoco TODO

What is **open**. Completed work is in `CHANGELOG.md`, standing design choices
and their reasoning are in `docs/decisions.md`, and what works today is the
README's "Status". Keep them separate — this file once carried all of
it, and the open list got lost inside the done one.

**On the bench (2026-10-02):** WROVER1 (MAX98357A, COM20) and WROVER2 (PCM5102
+ TPA3116, COM22) — the two that can be a server — the S3 (MAX98357A, COM9)
and the C3 (COM10). The WROOM is `esp32stereo`, a client at the stereo across
the room with no cable, updated over WiFi (D15) and asked over the mesh (D16). The
WROVERs' ports move with every replug: check the MAC (`?`) against
`boards.local.md`.

---

## Blocking

- [ ] **The server drops 3–13% of its own frames.** A WROVER streaming
      Bluetooth loses its own mesh frames to its BT link — singles, and runs
      of 4–5 when the link has the radio — and every client loses the same
      ones. Since 2026-10-06 the XOR of the blocks 1 and 15 back is the
      default (D13), and it leaves 0.3–1.1% of blocks as 2.6 ms holes, about
      45% fewer than the copy at 11 did on the same losses; the more the
      server drops, the more holes (`CHANGELOG.md`). Next, scored or not:
      a client that keeps the parities it could not use yet and retries them
      when a block comes back (`xor 15*` in `losstrace.py`: about 30% fewer
      holes again, on every trace so far), `t2` on top at twice the airtime
      (the trace cannot score it), and a **two-chip server** (D18, below),
      which removes the cause.
      Also unexplained: the server's own output crackled once on 2026-09-28
      (27 holes/s, one per A2DP packet) and never again after a reboot. If
      it comes back, run `listen.py --serial` at once and compare the `a`
      window with the clean ones in `CHANGELOG.md`.
- [ ] **Concealment: hear it, then make it the default.** A hole left after
      redundancy plays zeroes — a click. `z1` (2026-10-01,
      `lib/jitter/conceal.h`) fills it from both neighbours, each played
      backwards from the edge it shares, so there is no step at either edge;
      a run fades out, rests and fades in, and a block patched later replaces
      only its own fill. The tests show the step gone and the 600 s bench
      regression ran with it on. Left: the ear. Behind the streaming server
      with redundancy off (`D0`, so holes are frequent), one client unmuted
      at volume, `listen.py --client` alternating `z0` and `z1`, the clicks
      and dips counted. If it holds, `MESH_CONCEAL` 1.
- [ ] **The Bluetooth button (D17): run it on a WROVER.** Built for every
      environment on 2026-10-09 and never pressed. A node that has played as
      a client keeps Bluetooth off, and a short press opens it for 60 s. That
      is now also how a phone takes over a node that is playing, which used to
      fail ("Couldn't connect", 2026-09-28). On WROVER2, behind WROVER1
      streaming, with the join logger of 2026-10-06 on the clients:
      - After a stream and 5 s of silence, `N` says `bt=0`, and the next
        stream starts on WROVER2 in the same millisecond as on the S3 and C3.
      - A short press while it plays: the music stops, two drops, `bt=1`, and
        the phone finds it. Connecting plays the jingle and makes it SERVER,
        and the other clients stay with WROVER1 until it goes quiet.
      - A short press and no phone: after 60 s Bluetooth goes off (`bt=0`) and
        it rejoins the stream, with no falling drops. With nothing playing,
        the falling drops.
      - A long press still offers the mesh, and its release opens nothing.
      - `@SonoLoco-WROVER2 B` answers in time, with the drops coming after the
        answer, and `@* B` is refused. A bounce under 50 ms does nothing.
      Still open from before: a slower Bluetooth scan might let a client stay
      connectable while it plays, unmeasured (D17). And Windows once failed to
      connect to a freshly rebooted node with nothing reaching it, then
      connected fine later; not understood.
- [ ] **The two-chip server (D18): run it.** Built on the `two-chip-server`
      branch on 2026-11-12, never on a board. A WROOM in front-end mode (`F`)
      wired to the S3: the WROOM's link TX (GPIO 33) to the S3's link RX
      (GPIO 16), the S3's TX (17) to the WROOM's RX (32), and ground. In order:
      - The link alone, no phone: `s` on the front end sends the bench tone
        over the link at A2DP's pace. The S3 becomes SERVER (`N` says
        `src=link`), `>a` shows no CRC errors and no sequence gaps, and the
        clients play the tone with the usual bench-regression counters.
      - CPU: the S3's `a` window and its clients' `und`/`dry` with the link at
        `LINK_BAUD` against a bench run without it.
      - A phone on the front end, 600 s behind it with `soak.py --trace`:
        the clients' `lost` against the 3–13% a WROVER server drops today. The
        number this whole design is for.
      - Two radios in one box: the same run with the boards' antennas apart
        and side by side.
      - The C3 as the node, single core: the same link run, `und` and `dry`.
      Then merge, and decide whether WROVERs keep their own Bluetooth path.

---

## Open design issues

### Sound

- [ ] **Hear the startup sound on an S3 and a C3.** Every node plays it on a
      power-on since 2026-10-01, at `TONE_AMPLITUDE` 4000 on the MAX98357A
      nodes, but a USB flash resets an S3 or C3 through USB, not as a
      power-on. Unplug one and plug it back in. A short "joined the mesh"
      tone on entering CLIENT would fit the same mechanism.
- [ ] **Hear a jingle during a stream.** The connect and disconnect jingles
      are written from `loop()` into the server's own output, which `loop()`
      also feeds from the ring, so they cannot interleave (D14). Left to
      check: `listen.py --serial COM20 --at 3.0:j` — the notes whole, the
      stream resuming after, and the clients jumping onto the server's new
      schedule once (`sjmp=` +1) rather than repeatedly.
- [ ] **WROVER2's TPA3116 is too hot for its speaker**: very loud by 50% on
      the phone. Since 2026-10-01 the per-node trim does it in firmware —
      `v-12` on WROVER2 turns only that room down, because it is applied
      after the mesh has its copy. Or on the amp: the gain jumper, or a
      divider between DAC and amp input (10 k series, 3.3 k to ground ≈
      −12 dB).
- [ ] **Should the mesh carry audio before the phone's volume?** The volume
      is applied on the server before forwarding, so at a low phone volume
      the 16-bit stream has already lost bits no client can get back. The
      per-node trim is applied after, at each node, and does not change this.
      The alternative: forward full-scale audio plus the volume value, and
      apply both at every output.
- [ ] **A stereo pair.** Every client gets both channels. Two nodes in one
      room could play left and right — a per-node channel setting beside `M`.
      Nothing asked for it yet. It would also ask more of D14: proportional
      control parks each client `SYNC_DEADBAND_US` plus rate/kp off the
      schedule, on the side its drift pushes it, and two clients drifting
      opposite ways sat 0.7 ms apart on the bench. Between rooms that is
      nothing; between the two speakers of a pair it moves the image. A
      smaller deadband and a larger `SYNC_KP` with a shorter filter would
      narrow it — simulate that in `test_drift` before trusting it.

### Timing / sync

- [ ] **A per-node latency trim, if the microphone asks for one.** D14 puts
      every node on the server's schedule to the precision of the DMA
      interrupt, but not the DAC after it or the air: a PCM5102 and a
      MAX98357A may differ by a fraction of a millisecond, and two rooms by
      metres of sound. If `sync.py` shows a steady offset on one board, a
      trim in µs next to the volume trim is the place for it.
- [ ] **Sample rate is assumed to be 44.1 kHz.** If A2DP negotiates 48 kHz the
      clients play at the wrong pitch. Read the actual rate from the sink and
      either follow it or resample.

### Surviving the wild

The product is a mesh of speakers in somebody's flat, in a building full of
other people's routers. The bench is one room with one router, and even that
was enough to take 10.7% of the packets off the weakest client. Everything
here follows from three measured facts (2026-09-14, `CHANGELOG.md`):

1. **Loss is other people's traffic, not the mesh.** Four clients on one
   broadcast are clean when the channel is quiet. `tools/airmon` on a spare
   WROOM showed the band empty except channel 1 with the house router at
   -54 dBm, and every burst of client loss lined up with a burst of
   undecodable frames at the monitor — a Wi-Fi 6 router streaming to a
   phone, which an ESP32 can only count. Same afternoon, mesh moved to
   channel 11, same router traffic on channel 1: S3 and C3 zero lost.
2. **Broadcast has no retransmission.** A frame lost in the air is a hole in
   the audio unless a later packet carries it (D13).
3. **Airtime is the lever the mesh actually controls.** 1 → 6 Mbps cut the
   loss under the same interference 4.6× (D13). Shorter frames collide less.

- [ ] **Channel agility — necessary, not sufficient.** In a seven-storey
      building every channel carries something; the best a survey can do is
      pick the least loaded one, and it should: on this bench that was the
      difference between 837 lost and 0. At boot the source surveys (the
      survey in `tools/airmon/src/main.cpp` is the measurement half), picks
      the emptiest of 1/6/11 — or of all 13 — and announces it; clients scan
      for the mesh's beacon instead of sitting on `ESPNOW_CHANNEL`. Re-survey
      only between streams, never while audio flows. Two meshes then end up
      on different channels for free whenever the survey says so, and the
      id keeps them apart when it does not. **What it does not do:** make
      the least-loaded channel quiet. Frequency *hopping* was considered and
      rejected: with a known static interferer a hop schedule visits the bad
      channel 1/13 of the time, the receiver is deaf for about a millisecond
      per hop, a client that loses the schedule has to rescan, and a WROVER
      server is already hopping its BT radio under the coexistence arbiter.
      Hopping is for a band you cannot survey; this one can be surveyed.
- [ ] **Redundancy against somebody else's traffic.** Tuned so far against a
      Bluetooth server's own radio; a loaded router loses frames in longer
      bursts. Record a loss trace with the router deliberately loaded (a
      phone video is a repeatable enough "wild") and the monitor watching,
      and score it with `losstrace.py` — the same tool, a different enemy.
- [ ] **Buffer for bursts.** A deeper jitter buffer rides out longer bursts
      at the cost of latency. Who waits is decided (D14): the server plays
      through a ring as deep as its clients', so a deeper `JITTER_PREFILL`
      delays every room together, the server included, and costs only
      lip-sync against the phone.
- [ ] **Smaller frames.** ADPCM spent its 4:1 on stereo, full rate and a
      redundant block (D5, D13), not on short frames: a packet is 248 bytes,
      about 0.2 ms at 12 Mbps. Under heavy interference shorter would collide
      less — 114-frame blocks could be halved at the cost of twice the
      packets and headers, or a household could choose mono. Measure under
      load before trading audio for it.
- [ ] **Test in an actual building.** Take the boards and the monitor to a
      real flat. Survey first, then a 600 s run on the channel the survey
      picks, with the monitor logging beside it, and keep both logs. Until
      then every number in this file is one room, one router.
- [ ] **Range-test 12 Mbps further than one room.** Across one room, to the
      stereo, 240,425 packets and none lost at −52 dBm (2026-10-01). 12 Mbps
      is about 4 dB less sensitive than 6 and 9 dB less than 1, and sending
      every block twice buys some of that back. Walk a client into the next
      room and the one after, 600 s each at 12 and at 6 (`R<Mbps>` switches
      a source at runtime), with `rec` and `lost` on the client — or ask the
      stereo node from wherever it stands (`ota.ps1 -Status`). 2 Mbps is not
      the fallback: behind a streaming BT server every frame waited 20–50 ms
      for the radio and the client dropped out (D13).

### Mesh isolation

Separation itself is **done** — every packet carries a 16-bit mesh id and a
client drops anything that is not its own (D12). What is left is proof and
polish.

- [ ] **Prove it with two meshes in the air.** Nothing here has been tested
      with an actual second household. `docs/bench-test.md` has the run:
      the source and one client on `gcasa rossi`, the odd one out on the
      default reporting `rx=0` with `fgn=` climbing at ~387/s, the matched
      pair as clean as a one-mesh run.
- [ ] **Pairing has never been pressed.** Both halves are written and compile,
      and no finger has been on either. What to check, on a board with a DAC so
      the tones can be heard:

      - A hold of 3-9 s, let go, opens a window at each end — one drop as the
        hold reaches 3 s, two drops on release, and the matching
        `[MESH] offering` / `[MESH] listening` line on the console. Since D19
        it acts on release, not at 3 s.
      - Offer on one, listen on the other: the listening node adopts within a
        second, plays three rising drops, and comes back streaming on the new mesh.
      - **The negative case, which is the whole reason there are two presses.**
        With no offer open, a listening node must sit through a foreign stream
        and report `listening closed, no offer heard` after 60 s, having joined
        nothing. If it joins anyway, the beacon check is not doing its job.
      - The adopted id survives a **power cut**, not merely a reset — NVS is the
        whole reason to prefer it over RTC memory.
      - The button on both a C3 and a classic board: GPIO 9 there, GPIO 0
        elsewhere, and only one of those has ever been reasoned about twice.
      - A beacon arriving at a node **already on that mesh** changes nothing and
        charges no `rsy` — it is dropped before the sequence tracker, and a
        regression there would be an audible re-arm caused by a node saying
        hello.
- [ ] **Decide whether an idle server should be discoverable at all.** Pairing
      needs the offering node to be reachable, and today it is: a beacon is sent
      whether or not music is playing. But a node only *streams* while audio
      flows, so "is my server working?" still has no answer until somebody plays
      something. A slow idle beacon — one a second, say, outside pairing too —
      would let a client show that its mesh exists. Weigh it against the radio
      time it costs and against giving a neighbour a constant signal to see.
- [ ] **The setup network (D19): run it.** Built on the `config-mode` branch
      on 2026-11-12, on top of `two-chip-server`; never on a board. The page's
      script was run in a browser against a fake node (every control, a refused
      rename, a node that never answers, the password and WiFi forms), the
      firmware not at all. First run, 2026-11-17, WROVER1 and the stereo
      WROOM: every command over the mesh as designed (read-back, `m1` twice,
      rename, retries, `*` refusals, one-node answers in 0.33 s); the page
      listed both nodes; Ring rebooted the WROVER (`docs/gotchas.md`), fixed.
      Still to see on a phone: the password flow end to end, and Ring. On a
      board, in order:
      - The S3, freshly flashed with no password: `[CONFIG] open ... auth=open`
        at boot, two drops, and `SonoLoco-S3-setup` in a phone's list. Joining
        it opens the page by itself on Android and on an iPhone (the captive
        DNS); if one does not, 192.168.1.1 by hand. The phone gets an
        address in 192.168.1.x from the node.
      - The page lists every node (`@* N`); trim, mute, mono and rename on a
        client each answer, and the room hears it.
      - Ring (`h`): two ding-dongs on that node only — over its music while it
        plays (mixed in after trim and mute), on its own in DISCOVERY, and
        through a mute. Its `und`/`dry` unchanged by it, and the bench
        regression unchanged with no ring asked: the mix is in the output path. A node switched off shows
        "no answer, after retries" and its control goes back.
      - **The mesh while a phone is on it**: a 600 s bench run with the S3 as a
        client and a phone joined to its network, against the latest bench
        regression. The AP and ESP-NOW share the channel; this is the number
        that says whether that costs anything.
      - The password: set it, the network comes back WPA2, the old open one is
        gone, and the home WiFi form appears. A power cut keeps the password
        and does not open the network at boot again.
      - A WROVER: the 10-15 s hold pauses Bluetooth (`bt=0`) and opens the
        network; `C0` restores Bluetooth. Refused while a phone is connected.
      - The window closes by itself 5 minutes after the page's last request,
        and with the page's close button, with three falling drops.
- [ ] **The setup page over the home WiFi.** Nodes that join the router so the
      page is reachable by IP, without leaving the home network. It moves the
      mesh to the router's channel, where the bench lost 837 packets against 0
      on channel 11 before redundancy: measure the loss there first (D19).
- [ ] **Front ends and the setup page.** A phone sees a front end's Bluetooth
      name, which is its own build's `ROOM_NAME`: the node should hand its NVS
      name to its front end over the link (D18).
- [ ] **Payload encryption, if privacy ever matters.** The mesh id keeps a
      neighbour's player out, not a neighbour's receiver: ESP-NOW encrypts only
      unicast frames (per-peer LMK), and this design is broadcast by
      construction, so anyone running modified firmware can still listen. The fix
      is encrypting the payload under a key derived from the mesh name, on a
      client that already runs I2S, drift correction and a radio — measure before
      believing it fits.

### Housekeeping

- [ ] **Set `board_build.arduino.memory_type = qio_opi` for the S3** so its 8 MB
      embedded PSRAM is actually usable. `esptool` reports the PSRAM, the
      firmware reports `psram=0`. Nothing needs it yet, so this is only about the
      logs being honest.
- [ ] Rename `esp32-code/` to something consistent with the project name.

---

## EMI / "frying" noise (hardware, unchanged)

- [ ] I2S wire lengths <10 cm, twist BCK/WS/DATA together
- [ ] 100nF + 10µF decoupling close to ESP32 VCC and PCM5102 VCC
- [ ] Single-point grounding across ESP32 / PCM5102 / TPA3116
- [ ] Ferrite beads on I2S lines if noise persists
- [ ] Separate 3.3V LDO for analog vs digital

---

## Roadmap (not started)

- [ ] ESPectre presence detection — auto-pause when a room is empty
- [ ] Dynamic volume based on presence
- [ ] Home Assistant integration
