"""How far apart the nodes play: clicks into the server, one node heard at a time.

    python tools/btlisten/sync.py --server COM20 --node COM9 [--node COM22:-24] [--volume 110]
    python tools/btlisten/sync.py --analyze logs/sync-....wav

The PC streams a train of short clicks into the Bluetooth server and records
with its microphone. The recording is cut into phases, and in each phase only
one node is unmuted: the server first, then every --node in turn, then the
server again. Every phase is part of one Bluetooth stream, so the PC's own
latency is the same throughout and cancels -- what is left between two phases
is the difference between the two nodes, plus the sound's flight time from
each speaker to the mic (~2.9 ms a metre).

The clicks are spaced irregularly (300-500 ms), so a search for the delay
cannot lock onto the click before or after: only the right delay lines up the
whole phase. The server is heard again at the end, and the two server phases
should agree to a fraction of a millisecond -- if they do not, the reference
moved during the run and the offsets between are not worth quoting.

The nodes differ by 20 dB and more (a MAX98357A against a TPA3116), and the
server's A2DP volume is the only volume there is: it is applied before
forwarding, so it moves every node. --volume sets it for the run and puts
back what it was; `--node COM22:-24` plays that node's clicks 24 dB quieter,
so the MAX98357A nodes can be loud enough for the mic without the TPA3116
being deafening.

Mutes are read, never toggled blind: `?` gives each node's `mute=`, `m` is
sent only where the state has to change and its reply is checked, and every
node is put back as it was found. The nodes' `und` (underruns) are compared
before and after -- a client that re-armed mid-run changed its own delay, and
the tool says so.

Recordings go to logs/sync-<time>[-label].wav with a .json of the click times
and phases beside it, so --analyze can redo the numbers later.
"""
import argparse, datetime, json, os, re, sys, threading, time
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import listen, ser

ROOT = listen.ROOT
FS_OUT = 44100


def click(fs, freq=2500.0, cycles=3):
    """A few cycles of a tone under a Hann window: short, and not a pure impulse,
    which a small speaker would turn into its own ringing."""
    n = int(round(cycles / freq * fs))
    t = np.arange(n) / fs
    return np.sin(2 * np.pi * freq * t) * np.hanning(n)


def make_plan(names, clicks, lead, gap, seed):
    """Phases in playback time: (name, [click times]), and when to switch each mute."""
    rng = np.random.default_rng(seed)
    order = list(names) + [names[0]]
    t = lead
    phases = []
    for name in order:
        t += gap
        times = []
        for _ in range(clicks):
            times.append(t)
            t += rng.uniform(0.30, 0.50)
        phases.append((name, times))
    return phases, t + gap


def make_signal(phases, total, level_db, trims):
    x = np.zeros(int(total * FS_OUT))
    for name, times in phases:
        c = 10 ** ((level_db + trims.get(name, 0.0)) / 20) * click(FS_OUT)
        for t in times:
            i = int(round(t * FS_OUT))
            x[i:i + len(c)] += c
    return np.stack([x, x], axis=1).astype(np.float32)


def read_mute(sp):
    sp.reset_input_buffer()
    for line in ser.talk(sp, '?', 0.3, show=False):
        m = re.search(r'\bmute=(\d)', line)
        if m:
            return m.group(1) == '1'
    return None


def set_mute(sp, want, known):
    """Toggle only if `known` (the state last read or confirmed) says it has to,
    and take the state from the reply. Returns the state now held."""
    if known == want:
        return known
    sp.reset_input_buffer()
    for line in ser.talk(sp, 'm', 0.2, show=False):
        m = re.search(r'\[OUT\] mute=(\d)', line)
        if m:
            return m.group(1) == '1'
    return read_mute(sp)


def volume(sp, set_to=None):
    """The server's A2DP volume; `V<n>` sets it, a bare `V` only reports it."""
    sp.reset_input_buffer()
    cmd = 'V' + ('' if set_to is None else str(set_to)) + '\n'
    for line in ser.talk(sp, cmd, 0.4, show=False):
        m = re.search(r'\[A2DP\] volume=(\d+)', line)
        if m:
            return int(m.group(1))
    return None


def counters(sp):
    sp.reset_input_buffer()
    for line in ser.talk(sp, 'r', 0.4, show=False):
        if line.startswith('[BENCH] ms='):
            return dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
    return {}


def envelope(x, fs):
    """Band-limited energy envelope around the click, for finding arrivals."""
    from scipy import signal
    sos = signal.butter(4, [1200, 6000], 'bandpass', fs=fs, output='sos')
    y = signal.sosfiltfilt(sos, x)
    tpl = click(fs)
    c = signal.fftconvolve(y, tpl[::-1], mode='same')
    return np.abs(signal.hilbert(c))


def analyze(x, fs, plan, lag_min=-1.0, lag_max=2.0):
    env = envelope(x, fs)
    noise = np.median(env)
    phases = plan['phases']
    t_rec0 = plan['record_lead']          # recording time of playback time 0
    out = []
    for name, times in phases:
        times = np.asarray(times) + t_rec0
        # Coarse: the delay that lines up the whole phase.
        # Wide, and negative too: when playback really starts against the
        # recording is not known to better than ~100 ms, and it does not need
        # to be -- only the differences between phases are reported.
        lags = np.arange(lag_min, lag_max, 1 / fs)
        idx = (times[:, None] * fs + lags[None, :] * fs).astype(int)
        idx = np.clip(idx, 0, len(env) - 1)
        score = np.log(env[idx] + noise).sum(0)
        lag = lags[np.argmax(score)]
        # Fine: each click's own peak within +-4 ms of that.
        per = []
        for t in times:
            i0 = int((t + lag - 0.004) * fs)
            i1 = int((t + lag + 0.004) * fs)
            seg = env[i0:i1]
            if len(seg) == 0:
                continue
            k = np.argmax(seg)
            snr = 20 * np.log10(seg[k] / noise)
            if snr < 12:
                continue
            # Parabolic interpolation of the peak, for sub-sample timing.
            if 0 < k < len(seg) - 1:
                a, b, c = seg[k - 1], seg[k], seg[k + 1]
                k = k + 0.5 * (a - c) / (a - 2 * b + c)
            per.append((i0 + k) / fs - t)
        out.append((name, lag, np.array(per)))
    return out


def report(res):
    ref = None
    print(f'{"phase":24s} {"clicks":>6s} {"delay ms":>9s} {"spread":>7s} {"vs server":>10s}')
    for name, lag, per in res:
        if len(per) == 0:
            print(f'{name:24s}  none found -- muted, or too quiet at the mic')
            continue
        d = np.median(per) * 1000
        if ref is None:
            ref = d
        spread = (per.max() - per.min()) * 1000
        print(f'{name:24s} {len(per):6d} {d:9.2f} {spread:7.2f} {d - ref:+10.2f}')
    first = [r for r in res if len(r[2])]
    if len(first) >= 2 and first[0][0] == first[-1][0]:
        drift = (np.median(first[-1][2]) - np.median(first[0][2])) * 1000
        print(f'reference moved {drift:+.2f} ms between the two {first[0][0]} phases'
              + ('' if abs(drift) < 1.0 else '  -- NOT stable, offsets are suspect'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--analyze', metavar='WAV', help='redo the numbers for an earlier run')
    ap.add_argument('--server', metavar='COMn', help='the Bluetooth server: the reference node')
    ap.add_argument('--node', metavar='COMn[:dB]', action='append', default=[],
                    help='a client to measure against it, optionally with its clicks '
                         'that many dB quieter or louder; repeat for each')
    ap.add_argument('--volume', type=int, help="the server's A2DP volume (0-127) for the "
                    "run; the one found is put back afterwards")
    ap.add_argument('--device', default='SonoLoco', help='output endpoint name (substring)')
    ap.add_argument('--mic', default='Microphone', help='input endpoint name (substring)')
    ap.add_argument('--level', type=float, default=-20.0, help='click peak, dBFS')
    ap.add_argument('--clicks', type=int, default=8, help='clicks per phase')
    ap.add_argument('--label', default='')
    ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()

    if a.analyze:
        x, fs = listen.load_wav(a.analyze)
        with open(a.analyze[:-4] + '.json') as f:
            plan = json.load(f)
        report(analyze(x, fs, plan))
        return
    if not a.server or not a.node:
        ap.error('--server and at least one --node are needed')

    trims = {}
    nodes = []
    for spec in a.node:
        port, _, db = spec.partition(':')
        nodes.append(port)
        trims[port] = float(db) if db else 0.0
    a.node = nodes
    ports = [a.server] + a.node
    names = [f'{p} (server)' if p == a.server else p for p in ports]
    # A long lead: the stream starts with the playback, and a client joining it
    # arms (and may re-arm once) in the first seconds. Measure after that.
    lead, gap = 4.0, 2.0
    phases, total = make_plan(names, a.clicks, lead, gap, a.seed)
    sig = make_signal(phases, total, a.level, trims)

    sps = {p: ser.open_port(p) for p in ports}
    initial = {p: read_mute(sps[p]) for p in ports}
    if any(v is None for v in initial.values()):
        sys.exit(f'no mute= from {[p for p, v in initial.items() if v is None]} -- '
                 'old firmware, or not answering')
    print('mute as found: ' + ' '.join(f'{p}={int(v)}' for p, v in initial.items()))
    vol0 = None
    if a.volume is not None:
        vol0 = volume(sps[a.server])
        if vol0 is None:
            sys.exit('the server did not report its volume')
        print(f'server volume {vol0} -> {volume(sps[a.server], a.volume)} for the run')
    c0 = {p: counters(sps[p]) for p in a.node}

    # Everyone muted before the first phase; then, in each gap, the phase's node
    # on and the rest off. The switch is 0.6 s into the gap: the last
    # click of the phase before has been played by then, the first of the next
    # has not -- the PC's Bluetooth path adds a few hundred ms to both.
    state = dict(initial)
    for p in ports:
        state[p] = set_mute(sps[p], True, state[p])
    log = []

    def switch(k):
        want = ports[names.index(phases[k][0])]
        for p in ports:
            if p != want:
                state[p] = set_mute(sps[p], True, state[p])
                if state[p] is not True:
                    log.append(f'{p} did not mute before phase {k}')
        state[want] = set_mute(sps[want], False, state[want])
        if state[want] is not False:
            log.append(f'{want} did not unmute for phase {k}')

    def on_start():
        for k, (_, times) in enumerate(phases):
            threading.Timer(times[0] - gap + 0.6, switch, args=(k,)).start()

    print(f'{datetime.datetime.now():%H:%M:%S} {len(sig) / FS_OUT:.1f} s of clicks at '
          f'{a.level:.0f} dBFS to {a.device!r}, phases: ' + ', '.join(n for n, _ in phases))
    x, fs = listen.record(sig, FS_OUT, a.device, a.mic, tail=1.0, on_start=on_start)

    for p in ports:
        got = set_mute(sps[p], initial[p], read_mute(sps[p]))
        if got != initial[p]:
            log.append(f'{p} NOT restored to mute={int(initial[p])}, now {got}')
    if vol0 is not None:
        print(f'server volume restored to {volume(sps[a.server], vol0)}')
    c1 = {p: counters(sps[p]) for p in a.node}
    print('mute restored: ' + ' '.join(f'{p}={int(bool(read_mute(sps[p])))}' for p in ports))
    for p in a.node:
        und = int(c1.get(p, {}).get('und', 0)) - int(c0.get(p, {}).get('und', 0))
        mode = c1.get(p, {}).get('mode', '?')
        print(f'{p}: mode={mode} und=+{und} jit={c1.get(p, {}).get("jit", "?")}'
              + ('  -- re-armed during the run, its delay changed' if und else ''))
    for sp in sps.values():
        sp.close()
    for l in log:
        print('WARNING:', l)

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    base = os.path.join(ROOT, 'logs', f'sync-{stamp}' + (f'-{a.label}' if a.label else ''))
    os.makedirs(os.path.dirname(base), exist_ok=True)
    listen.save_wav(base + '.wav', x, fs)
    # listen.record opens the mic 0.3 s before playback starts.
    plan = {'phases': phases, 'record_lead': 0.3, 'level': a.level, 'trims': trims,
            'volume': a.volume}
    with open(base + '.json', 'w') as f:
        json.dump(plan, f)
    print(f'saved {os.path.relpath(base, ROOT)}.wav, mic peak {np.abs(x).max():.2f}')
    report(analyze(x, fs, plan))


if __name__ == '__main__':
    main()
