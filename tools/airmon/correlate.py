"""Lay an air-monitor log next to a bench log, second by second.

    python tools/airmon/correlate.py logs/air-<stamp>.log logs/bench-<stamp>.log

Both logs carry PC wall-clock time (the bench log's header has its start time
and its lines are offsets from it), so the two are joined on the second. For
each second: the monitor's airtime, foreign data frames and bytes, FCS-failed
frames, and ESP-NOW frames it decoded, against every client's lost packets.
Then the correlation of each client's loss with the foreign traffic, and the
seconds that hurt most.
"""
import sys, re, datetime
from collections import defaultdict

air_path, bench_path = sys.argv[1], sys.argv[2]

# --- air log: HH:MM:SS.mmm [AIR] ... -----------------------------------------
air = {}
day = None
for line in open(air_path, encoding='utf-8'):
    m = re.match(r'# .*started (\d{4}-\d{2}-\d{2}) ', line)
    if m: day = m.group(1)
    m = re.match(r'(\d\d):(\d\d):(\d\d)\.\d+ \[AIR\] (.*)', line)
    if not m: continue
    sec = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))
    kv = dict(re.findall(r'(\w+)=([-\d./%]+)', m.group(4)))
    air[sec] = {
        'air': float(kv['air'].rstrip('%')), 'bad': int(kv['bad']), 'espnow': int(kv['espnow']),
        'data': int(kv['data']), 'databytes': int(kv['databytes']), 'beacons': int(kv['beacons']),
        'rssi': kv['en_rssi'].split('/')[0],
    }

# --- bench log: offsets from the header's start time -------------------------
start = None
lost = defaultdict(dict)   # port -> sec -> cumulative lost
rx = defaultdict(dict)
for line in open(bench_path, encoding='utf-8-sig'):
    m = re.match(r'# started\s*:\s*(\d{4}-\d{2}-\d{2}) (\d\d):(\d\d):(\d\d)', line)
    if m:
        start = int(m.group(2)) * 3600 + int(m.group(3)) * 60 + int(m.group(4)); continue
    m = re.match(r'\s*([\d.]+)\s+(COM\d+)\s+\[BENCH\] .*?role=SINK.*? rx=(\d+) lost=(\d+) .*?maxalloc=', line)
    if not m or start is None: continue
    sec = start + int(float(m.group(1)))
    lost[m.group(2)][sec] = int(m.group(4))
    rx[m.group(2)][sec] = int(m.group(3))

ports = sorted(lost)
secs = sorted(set(air) & set.intersection(*[set(lost[p]) for p in ports]))
if not secs:
    sys.exit('no overlapping seconds between the two logs')

def per_sec(d, s):
    prev = d.get(s - 1)
    return d[s] - prev if prev is not None else 0

rows = []
for s in secs:
    a = air[s]
    rows.append((s, a, {p: per_sec(lost[p], s) for p in ports}))

def corr(xs, ys):
    n = len(xs); mx = sum(xs) / n; my = sum(ys) / n
    sxy = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    sxx = sum((x - mx) ** 2 for x in xs); syy = sum((y - my) ** 2 for y in ys)
    return sxy / (sxx * syy) ** 0.5 if sxx and syy else float('nan')

print(f"{len(rows)} overlapping seconds  ({datetime.timedelta(seconds=secs[0])} .. {datetime.timedelta(seconds=secs[-1])})")
print()
hdr = f"{'':>10} {'air%':>6} {'data':>5} {'kB':>6} {'bad':>4} {'espnow':>6} " + ' '.join(f'{p:>6}' for p in ports)
print("Foreign traffic buckets (seconds in bucket, mean loss per client per second):")
print(hdr)
buckets = [(0, 0), (1, 9), (10, 49), (50, 199), (200, 10**9)]
for lo, hi in buckets:
    sel = [r for r in rows if lo <= r[1]['data'] <= hi]
    if not sel: continue
    n = len(sel)
    label = f"data {lo}-{hi if hi < 10**9 else '+'}"
    print(f"{label:>10} {sum(r[1]['air'] for r in sel)/n:6.1f} {sum(r[1]['data'] for r in sel)/n:5.0f} "
          f"{sum(r[1]['databytes'] for r in sel)/n/1000:6.1f} {sum(r[1]['bad'] for r in sel)/n:4.1f} "
          f"{sum(r[1]['espnow'] for r in sel)/n:6.0f} " +
          ' '.join(f"{sum(r[2][p] for r in sel)/n:6.2f}" for p in ports) + f"   n={n}")
print()
print("Correlation of per-second loss with:")
for name, key in [('foreign data frames', 'data'), ('foreign data bytes', 'databytes'), ('airtime %', 'air'), ('FCS-failed frames', 'bad')]:
    xs = [r[1][key] for r in rows]
    print(f"  {name:20s} " + '  '.join(f"{p} {corr(xs, [r[2][p] for r in rows]):+.2f}" for p in ports))
print()
print("Worst seconds by total client loss:")
rows.sort(key=lambda r: -sum(r[2].values()))
for s, a, l in rows[:12]:
    t = str(datetime.timedelta(seconds=s))
    print(f"  {t}  air={a['air']:5.1f}% data={a['data']:4d} kB={a['databytes']/1000:6.1f} bad={a['bad']:3d} espnow={a['espnow']:3d} rssi={a['rssi']:>4}  " +
          ' '.join(f"{p}={l[p]}" for p in ports))
