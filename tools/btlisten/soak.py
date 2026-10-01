"""A long, silent run behind the Bluetooth server: what breaks, and how often.

    python tools/btlisten/soak.py --server COM20 --node COM9 --node COM22 [--secs 600]

The PC streams a quiet tone into the server for --secs, with every node
muted -- the mute is on each node's output only, so the stream, the ring and
the schedule run exactly as they would out loud. Every 10 s each client's
telemetry (`r`) and the server's A2DP window (`a`) are read *while the stream
is still running*: counters read after it, as sync.py does, include the
underrun every node has when the stream stops.

Reports, per node, the underruns (`und`, a DMA that ran dry and re-armed),
`dry`, the schedule jumps (`sjmp`), the range of the timing error (`se`, us),
and on clients the blocks lost and rebuilt. Mutes are read before they are
toggled and put back as found. The log goes to logs/soak-<time>.log.

With --trace each client also records which packets it missed (`L1`), into
the same log, and losstrace.py scores every redundancy scheme on it.
"""
import argparse, datetime, os, re, sys, threading, time
import numpy as np
import sounddevice as sd

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import listen, ser, sync

KEYS = ('rx', 'lost', 'rec', 'ovf', 'und', 'dry', 'sjmp', 'se', 'sync', 'jit', 'mode', 'ins', 'drp')


def a_window(sp):
    sp.reset_input_buffer()
    for line in ser.talk(sp, 'a', 0.5, show=False):
        if line.startswith('[A2DP] win='):
            return dict(re.findall(r'(\w+)=(\S+)', line)), line
    return {}, ''


def node_lines(sp, cmd, secs, sink):
    """talk() without discarding what came in since the last read: with the
    trace on, that is ten seconds of [LT] lines, which go to `sink`."""
    rest = []
    for line in ser.talk(sp, cmd, secs, show=False):
        if line.startswith('[LT] t='):
            sink(line)
        else:
            rest.append(line)
    return rest


def node_counters(sp, sink):
    for line in node_lines(sp, 'r', 0.4, sink):
        if line.startswith('[BENCH] ms='):
            return dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
    return {}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--server', required=True, metavar='COMn')
    ap.add_argument('--node', action='append', default=[], metavar='COMn')
    ap.add_argument('--secs', type=float, default=600)
    ap.add_argument('--device', default='SonoLoco')
    ap.add_argument('--level', type=float, default=-40.0, help='tone level, dBFS')
    ap.add_argument('--trace', action='store_true',
                    help='record which packets each client missed (L1), for losstrace.py')
    a = ap.parse_args()

    ports = [a.server] + a.node
    sps = {p: ser.open_port(p) for p in ports}
    if a.trace:   # ten seconds of trace between reads must not overflow the driver
        for p in a.node:
            if hasattr(sps[p], 'set_buffer_size'):
                sps[p].set_buffer_size(rx_size=1 << 16)
    initial = {p: sync.read_mute(sps[p]) for p in ports}
    if any(v is None for v in initial.values()):
        sys.exit(f'no mute= from {[p for p, v in initial.items() if v is None]}')
    for p in ports:
        sync.set_mute(sps[p], True, initial[p])

    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    path = os.path.join(listen.ROOT, 'logs', f'soak-{stamp}.log')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    log = open(path, 'w', encoding='utf-8')

    def out(s):
        print(s, flush=True)
        log.write(s + '\n'); log.flush()

    def tracer(port):
        def sink(line):   # the log only: a line a second per client is noise on screen
            log.write(f'{time.time() - t0:7.1f}s {port} {line}\n')
        return sink

    fid = [ser.talk(sps[p], '?', 0.4, show=False) for p in ports]
    for p, lines in zip(ports, fid):
        for l in lines:
            if 'fw=' in l:
                out(f'{p} {l}')

    fs = 44100
    amp = 10 ** (a.level / 20)
    phase = [0]

    def cb(outdata, frames, t, status):
        n = np.arange(phase[0], phase[0] + frames)
        outdata[:, 0] = outdata[:, 1] = (amp * np.sin(2 * np.pi * 997 * n / fs)).astype(np.float32)
        phase[0] += frames

    dev = listen.find_dev(a.device, 'output')
    series = {p: [] for p in a.node}
    srv = []
    t0 = time.time()
    out(f'{datetime.datetime.now():%H:%M:%S} streaming {a.secs:.0f} s at {a.level:.0f} dBFS, nodes muted')
    with sd.OutputStream(device=dev, channels=2, samplerate=fs, dtype='float32', callback=cb,
                         extra_settings=sd.WasapiSettings(auto_convert=True)):
        time.sleep(8)                        # the stream starts, the clients join and arm
        for p in ports:                       # zero the server's window, the clients' loss runs
            ser.talk(sps[p], 'a' if p == a.server else 'l', 0.3, show=False)
        if a.trace:
            for p in a.node:
                on = [l for l in ser.talk(sps[p], 'L1\n', 0.3, show=False) if 'trace=1' in l]
                out(f'{p} trace ' + ('on' if on else 'DID NOT START'))
        while time.time() - t0 < a.secs:
            time.sleep(max(0.0, 10 - (time.time() - t0) % 10))
            el = time.time() - t0
            for p in a.node:
                c = node_counters(sps[p], tracer(p)) if a.trace else sync.counters(sps[p])
                series[p].append(c)
                out(f'{el:6.0f}s {p} ' + ' '.join(f'{k}={c.get(k, "?")}' for k in KEYS))
            w, line = a_window(sps[a.server])
            srv.append(w)
            out(f'{el:6.0f}s {a.server} {line}')
        if a.trace:   # what the last second left, then off, while the stream still runs
            time.sleep(1.2)
            for p in a.node:
                node_lines(sps[p], 'L0\n', 0.3, tracer(p))
    time.sleep(1)

    for p in ports:
        got = sync.set_mute(sps[p], initial[p], sync.read_mute(sps[p]))
        if got != initial[p]:
            out(f'WARNING {p} not restored to mute={int(initial[p])}')
    out('mute restored: ' + ' '.join(f'{p}={int(bool(sync.read_mute(sps[p])))}' for p in ports))

    out('\n=== summary ===')
    for p in a.node:
        s = [c for c in series[p] if c.get('mode') == 'CLIENT']
        if len(s) < 2:
            out(f'{p}: not a client for the run')
            continue
        d = lambda k: int(s[-1].get(k, 0)) - int(s[0].get(k, 0))
        se = [int(c['se']) for c in s if 'se' in c and c.get('sync') == '1']
        out(f'{p}: {len(s)} samples over {10 * (len(s) - 1)} s  und=+{d("und")} dry=+{d("dry")} '
            f'sjmp=+{d("sjmp")} lost=+{d("lost")} rec=+{d("rec")} ovf=+{d("ovf")}  '
            + (f'se {min(se)}..{max(se)} us' if se else 'never on the schedule'))
    ws = [w for w in srv if w]
    dd = lambda k: int(ws[-1].get(k, 0)) - int(ws[0].get(k, 0)) if len(ws) > 1 else 0
    gap = max((float(w.get('gapmax', '0ms')[:-2]) for w in ws), default=0)
    # und, ovf and dry are running totals on the server; gapmax is per window.
    out(f'{a.server} (server): und=+{dd("und")} dry=+{dd("dry")} ovf=+{dd("ovf")} '
        f'gapmax={gap:.1f}ms (worst 10 s window)')
    for p in a.node:   # reset at the start; late = copies that came after their silence played
        for l in node_lines(sps[p], 'l', 0.4, tracer(p)):
            if l.startswith('[LOSS]'):
                out(f'{p} ' + l.split(' ', 1)[1].replace('runs=', 'lost runs 1..7,8+ = '))
    out(f'log: {os.path.relpath(path, listen.ROOT)}')
    if a.trace:
        out(f'score it: python tools/btlisten/losstrace.py {os.path.relpath(path, listen.ROOT)}')
    for sp in sps.values():
        sp.close()


if __name__ == '__main__':
    main()
