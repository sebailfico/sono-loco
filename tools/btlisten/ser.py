"""Send commands to a node and print what comes back, without resetting it.

    python tools/btlisten/ser.py COM19 [cmds] [seconds]

DTR/RTS are set false *before* the port opens: on the CH340 auto-reset circuit
DTR false while RTS is still asserted pulls EN low, which is a reset -- and a
reset drops the Bluetooth link under test.
"""
import sys, time
import serial

# The firmware logs arrows and the Windows console is cp1252: print what it
# can rather than die mid-run with UnicodeEncodeError.
try:
    sys.stdout.reconfigure(errors='replace')
except AttributeError:
    pass


def open_port(port):
    sp = serial.Serial()
    sp.port, sp.baudrate, sp.timeout = port, 115200, 0.1
    sp.dtr = False
    sp.rts = False
    sp.open()
    return sp


def talk(sp, cmds='', secs=1.0, show=True):
    out = []
    for c in cmds:
        sp.write(c.encode())
        time.sleep(0.05)
    t0 = time.time()
    buf = b''
    while time.time() - t0 < secs:
        buf += sp.read(4096)
    for line in buf.decode('utf-8', 'replace').splitlines():
        if line.strip():
            out.append(line)
            if show:
                print(line)
    return out


if __name__ == '__main__':
    sp = open_port(sys.argv[1])
    talk(sp, sys.argv[2] if len(sys.argv) > 2 else '', float(sys.argv[3]) if len(sys.argv) > 3 else 2.0)
    sp.close()
