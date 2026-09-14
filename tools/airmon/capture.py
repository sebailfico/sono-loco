"""Capture the air monitor's serial output to a timestamped log.

    python tools/airmon/capture.py COM15 [seconds] [--survey] [--channel N]

Each line is prefixed with the PC's wall-clock time, the same clock
tools/bench-mesh.ps1 stamps its log with, so the two can be laid side by side.
Writes logs/air-<stamp>.log and echoes to the console. Runs until the time is up
or Ctrl+C. Uses PlatformIO's Python, which has pyserial:
    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe
"""
import sys, time, os, datetime
import serial

port = sys.argv[1]
secs = float(sys.argv[2]) if len(sys.argv) > 2 and not sys.argv[2].startswith('--') else 1e9
survey = '--survey' in sys.argv
channel = None
if '--channel' in sys.argv:
    channel = int(sys.argv[sys.argv.index('--channel') + 1])

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
logdir = os.path.join(root, 'logs')
os.makedirs(logdir, exist_ok=True)
stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
path = os.path.join(logdir, f'air-{stamp}.log')

sp = serial.Serial(port, 115200, timeout=0.2)
sp.dtr = False; sp.rts = False
time.sleep(0.3)
sp.reset_input_buffer()
if channel:
    sp.write(f'c{channel}\n'.encode()); time.sleep(0.2)
if survey:
    sp.write(b's')

t0 = time.time()
with open(path, 'w', encoding='utf-8') as f:
    f.write(f'# SonoLoco air monitor, port {port}, started {datetime.datetime.now():%Y-%m-%d %H:%M:%S}\n')
    print(f'logging to {path}')
    buf = b''
    try:
        while time.time() - t0 < secs:
            buf += sp.read(4096)
            while b'\n' in buf:
                line, buf = buf.split(b'\n', 1)
                s = line.decode('utf-8', 'replace').rstrip('\r')
                if not s:
                    continue
                out = f'{datetime.datetime.now():%H:%M:%S.%f}'[:-3] + ' ' + s
                print(out, flush=True)
                f.write(out + '\n'); f.flush()
    except KeyboardInterrupt:
        pass
sp.close()
