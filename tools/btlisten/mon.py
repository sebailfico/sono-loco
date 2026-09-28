"""Log a server's A2DP timing every 2 s while someone uses it normally.

    python tools/btlisten/mon.py COM19 [seconds]

Sends `a` every two seconds -- print the window, start a new one -- and writes
each reply with PC time to logs/a2dp-<time>.log. Meant for running beside
real music: when somebody says "it crackled just then", the window covering
that moment says whether the Bluetooth task was late.
"""
import datetime, os, sys, time
import ser

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
path = os.path.join(root, 'logs', f'a2dp-{datetime.datetime.now():%Y%m%d-%H%M%S}.log')
os.makedirs(os.path.dirname(path), exist_ok=True)
sp = ser.open_port(sys.argv[1])
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 600
t0 = time.time()
print(f'logging to {path}')
with open(path, 'w', encoding='utf-8') as f:
    try:
        while time.time() - t0 < secs:
            for line in ser.talk(sp, 'a', 2.0, show=False):
                out = f'{datetime.datetime.now():%H:%M:%S} {line}'
                print(out, flush=True)
                f.write(out + '\n'); f.flush()
    except KeyboardInterrupt:
        pass
sp.close()
