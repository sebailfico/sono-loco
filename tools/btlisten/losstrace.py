"""Score every redundancy scheme on one recording of which packets were lost.

    python tools/btlisten/losstrace.py logs/soak-<time>.log [--port COM9]

A client with `L1` prints `[LT]` lines: a bit per packet, 1 = never heard
(lib/jitter/losstrace.h). soak.py --trace logs them with each node's port.
This replays the firmware's own rules on that record, for each scheme a
server can send (D13):

  copy d   each packet also carries the block d back (`D<d>`)
  xor d    each packet carries the XOR of blocks 1 and d back (`X<d>`), and
           the client rebuilds in one pass, as onEspNowRecv does: from the
           blocks it holds when each packet arrives
  xor d*   the same parity, if a client kept every parity it received and
           re-tried it whenever a block came back -- an upper bound, for
           whether that would be worth writing

A rebuilt block counts only if its silence was in the ring to patch (the last
MAX_GAP_FILL_PKTS of a run) and, for xor d*, arrived within --horizon packets.
Live, every scheme is measured in different minutes, whose loss can swing 3×;
here every scheme sees the same losses. What it cannot score is `t2`, which
changes the airtime and so the losses themselves.

With two or more clients it also says how much of the loss they share: a
packet every client missed never left the server.
"""
import argparse, re, sys
from collections import defaultdict

LINE = re.compile(r'(?:^|\s)(?:(?P<pc>\d+(?:\.\d+)?)s\s+)?(?:(?P<port>COM\d+)\s+)?'
                  r'\[LT\] t=(?P<t>\d+) s=(?P<s>\d+) n=(?P<n>\d+) drop=(?P<drop>\d+) (?P<hex>[0-9a-f]*)')
RATE = 44100 / 114                # packets a second: one 114-frame block each
FILL = 24                         # MAX_GAP_FILL_PKTS in config.h
MARGIN = 15                       # ESPNOW_LEN_DIST_MAX: every scheme gets a fair run-in and run-out


def parse(path, only=None):
    """{port: [(pc_s or None, start_seq, [bool lost] ...)]}, in file order."""
    lines = defaultdict(list)
    with open(path, encoding='utf-8', errors='replace') as f:
        for raw in f:
            m = LINE.search(raw)
            if not m:
                continue
            port = m.group('port') or '-'
            if only and port != only:
                continue
            n, hx = int(m.group('n')), m.group('hex')
            data = bytes.fromhex(hx)
            bits = [(data[k // 8] >> (k % 8)) & 1 == 1 for k in range(n)]
            pc = float(m.group('pc')) if m.group('pc') else None
            lines[port].append((pc, int(m.group('s')), bits, int(m.group('drop'))))
    return lines


def segments(entries):
    """Join lines whose seqs follow on into unbroken runs of outcomes."""
    segs = []
    for pc, s, bits, _ in entries:
        if segs and (segs[-1]['s'] + len(segs[-1]['lost'])) % 65536 == s:
            segs[-1]['lost'].extend(bits)
        else:
            segs.append({'pc': pc, 's': s, 'lost': list(bits)})
    return segs


def patchable(lost):
    """Which lost blocks had silence in the ring to write over: the last FILL of each run."""
    ok = [False] * len(lost)
    i, n = 0, len(lost)
    while i < n:
        if not lost[i]:
            i += 1
            continue
        j = i
        while j < n and lost[j]:
            j += 1
        for k in range(max(i, j - FILL), j):
            ok[k] = True
        i = j
    return ok


def copy_rebuilt(lost, ok, d):
    n = len(lost)
    return [lost[b] and ok[b] and b + d < n and not lost[b + d] for b in range(n)]


def xor_online(lost, ok, d):
    """onEspNowRecv's pass: each packet, on arrival, rebuilds the one of its pair it lacks."""
    n = len(lost)
    known = [False] * n
    rebuilt = [False] * n
    for p in range(n):
        if lost[p]:
            continue
        known[p] = True
        near, far = p - 1, p - d
        if far < 0:
            continue
        if known[near] != known[far]:
            miss = far if known[near] else near
            if ok[miss]:
                known[miss] = rebuilt[miss] = True
    return rebuilt


def xor_ideal(lost, ok, d, horizon):
    """Every parity kept and re-tried until nothing changes. A block counts if it
    was back within `horizon` packets of its own slot."""
    n = len(lost)
    INF = float('inf')
    when = [p if not lost[p] else INF for p in range(n)]   # packet index it was known by
    changed = True
    while changed:
        changed = False
        for p in range(d, n):
            if lost[p]:
                continue
            near, far = p - 1, p - d
            a, b = when[near], when[far]
            if (a == INF) == (b == INF):
                continue
            miss, other = (far, a) if b == INF else (near, b)
            t = max(p, other)
            if ok[miss] and t - miss <= horizon:
                when[miss] = t
                changed = True
    return [lost[b] and when[b] != INF for b in range(n)]


def runs_of(flags):
    """Histogram of run lengths of True: 1, 2, 3, 4+."""
    h = [0, 0, 0, 0]
    r = 0
    for f in flags + [False]:
        if f:
            r += 1
        elif r:
            h[min(r, 4) - 1] += 1
            r = 0
    return h


def score(segs, horizon):
    """{scheme: (holes, counted, hole runs)} over every segment's middle."""
    out = defaultdict(lambda: [0, 0, [0, 0, 0, 0]])
    loss_runs = [0] * 8
    for seg in segs:
        lost = seg['lost']
        n = len(lost)
        if n <= 2 * MARGIN:
            continue
        ok = patchable(lost)
        lo, hi = MARGIN, n - MARGIN
        r = 0
        for f in lost[lo:hi] + [False]:
            if f:
                r += 1
            elif r:
                loss_runs[min(r, 8) - 1] += 1
                r = 0
        schemes = {'none': [False] * n}
        for d in range(1, MARGIN + 1):
            schemes[f'copy {d}'] = copy_rebuilt(lost, ok, d)
        for d in range(2, MARGIN + 1):
            schemes[f'xor {d}'] = xor_online(lost, ok, d)
            schemes[f'xor {d}*'] = xor_ideal(lost, ok, d, horizon)
        for name, rebuilt in schemes.items():
            holes = [lost[b] and not rebuilt[b] for b in range(lo, hi)]
            o = out[name]
            o[0] += sum(holes)
            o[1] += hi - lo
            o[2] = [x + y for x, y in zip(o[2], runs_of(holes))]
    return dict(out), loss_runs


def absolute(segs, ref):
    """Each segment's packets as {absolute index: lost}, laps resolved by PC time
    against ref = (pc, seq) -- the server numbers packets the same for every client."""
    pc0, s0 = ref
    pts = {}
    for seg in segs:
        if seg['pc'] is None:
            return None
        pred = s0 + (seg['pc'] - pc0) * RATE
        lap = round((pred - seg['s']) / 65536)
        base = seg['s'] + 65536 * lap
        for k, l in enumerate(seg['lost']):
            pts[base + k] = l
    return pts


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('log')
    ap.add_argument('--port', help='only this node')
    ap.add_argument('--horizon', type=int, default=45,
                    help='packets a rebuilt block may come after its own (xor d* only); '
                         '45 is ~116 ms, inside the 136 ms prefill')
    ap.add_argument('--all', action='store_true', help='every distance, not the best few')
    a = ap.parse_args()

    by_port = parse(a.log, a.port)
    if not by_port:
        sys.exit(f'no [LT] lines in {a.log}')

    results = {}
    for port, entries in by_port.items():
        segs = segments(entries)
        sc, loss_runs = score(segs, a.horizon)
        drops = max(e[3] for e in entries)
        pk = sum(len(s['lost']) for s in segs)
        lost = sum(sum(s['lost']) for s in segs)
        results[port] = (segs, sc)
        print(f'{port}: {pk:,} packets in {len(segs)} segment(s), {lost:,} lost '
              f'({100 * lost / max(pk, 1):.2f}%), drop={drops}')
        print(f'  lost runs 1..7,8+ = ' + ','.join(map(str, loss_runs)))

    ports = list(results)
    names = list(next(iter(results.values()))[1])
    def pct(port, name):
        h, c, _ = results[port][1].get(name, (0, 0, None))
        return 100 * h / c if c else float('nan')

    if not a.all:   # the reference points, and the best few of each kind
        mean = lambda nm: sum(pct(p, nm) for p in ports) / len(ports)
        keep = {'none', 'copy 1', 'copy 11', 'xor 11', 'xor 11*'}
        for kind in ('copy ', 'xor '):
            cands = [nm for nm in names if nm.startswith(kind) and not nm.endswith('*')]
            keep.update(sorted(cands, key=mean)[:3])
            keep.update(nm + '*' for nm in sorted(cands, key=mean)[:1] if kind == 'xor ')
        names = [nm for nm in names if nm in keep]

    CELL = 36
    print('\nholes left, % of blocks, and their runs 1,2,3,4+:')
    print(f'  {"scheme":9}' + ''.join(f'{p:>{CELL}}' for p in ports))
    for nm in names:
        cells = []
        for p in ports:
            h, c, r = results[p][1][nm]
            cells.append(f'{h:7d} {100 * h / c if c else 0:5.2f}%   {",".join(map(str, r)):<18}')
        print(f'  {nm:9}' + ''.join(f'{x:>{CELL}}' for x in cells))

    if len(ports) >= 2:
        first = results[ports[0]][0]
        if first and first[0]['pc'] is not None:
            ref = (first[0]['pc'], first[0]['s'])
            maps = {p: absolute(results[p][0], ref) for p in ports}
            if all(maps.values()):
                both = set.intersection(*(set(m) for m in maps.values()))
                lost_all = sum(1 for i in both if all(maps[p][i] for p in ports))
                print(f'\nshared: {len(both):,} packets every client traced; '
                      f'{lost_all:,} lost by all ({100 * lost_all / max(len(both), 1):.2f}%)')
                for p in ports:
                    mine = sum(1 for i in both if maps[p][i])
                    print(f'  {p}: {mine:,} lost, {100 * lost_all / max(mine, 1):.0f}% of them '
                          f'by every client -- the server never sent them')


if __name__ == '__main__':
    main()
