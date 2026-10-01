"""Play a known tone into a SonoLoco server over Bluetooth and listen with the PC mic.

    python tools/btlisten/listen.py [--serial COM19] [--pre f] [--level -20] [--label x]
    python tools/btlisten/listen.py --analyze logs/listen-....wav

The PC is the A2DP source, so the whole loop runs without a phone: pair the
node with Windows once, connect it, and it shows up as an audio endpoint.

Signal: lead-in silence, a 997 Hz sine, then digital silence with the stream
still open. What the analysis reports:

  dips    the tone's own envelope falling >6 dB for a millisecond or more --
          a hole in the audio. Their spacing is the diagnostic: one A2DP packet
          from Windows is 1024 frames, 23.2 ms.
  clicks  broadband energy left after notching out the tone and its harmonics.
  pitch   the tone as recorded. 997.00 means the samples arrived in order at
          the right rate; a few Hz off means time was inserted or dropped.
  length  the tone as recorded, against the 6.0 s that was sent.

A run through the laptop's own speaker (--device "Speakers (Realtek") is the
control: 997.00 Hz, zero dips. Anything the node adds on top is the node's.

With --serial, the node's A2DP window (the `a` command) is reset as playback
starts and printed at the end, so each recording comes with the board's own
view of the same seconds. --pre sends commands first: `f` toggles forwarding
to the mesh, `w` stops WiFi until reboot. --at 3.5:j sends one mid-playback:
`j` plays the connect jingle as a connection would, `J` the old unguarded way.
--client COMn (repeatable) prints each mesh client's counters for the same
seconds: lost, rec, and the lengths of the lost runs.

Recordings go to logs/listen-<time>[-label].wav, with a plot of 250 ms of the
tone beside it. Needs numpy, scipy, sounddevice, pyserial and matplotlib; see
requirements.txt next to this file.
"""
import argparse, datetime, os, sys, threading, time, wave
import numpy as np
import sounddevice as sd

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def find_dev(name, kind):
    wasapi = [i for i, h in enumerate(sd.query_hostapis()) if 'WASAPI' in h['name']][0]
    for i, d in enumerate(sd.query_devices()):
        if d['hostapi'] == wasapi and name in d['name'] and d[f'max_{kind}_channels'] > 0:
            return i
    # A Bluetooth speaker that is paired but not connected has no WASAPI
    # endpoint at all, and Windows can report the device "connected" while the
    # audio profile is not -- check the board is in SERVER, not DISCOVERY.
    sys.exit(f'no WASAPI {kind} device matching {name!r} -- is it connected?')


def make_signal(fs, lead, tone, silence, freq, level_db):
    a = 10 ** (level_db / 20)
    n = int(tone * fs)
    s = a * np.sin(2 * np.pi * freq * np.arange(n) / fs)
    ramp = int(0.01 * fs)                      # 10 ms fades: no click of our own
    s[:ramp] *= np.linspace(0, 1, ramp)
    s[-ramp:] *= np.linspace(1, 0, ramp)
    x = np.concatenate([np.zeros(int(lead * fs)), s, np.zeros(int(silence * fs))])
    return np.stack([x, x], axis=1).astype(np.float32)


def record(sig, fs_out, out_name, mic_name, tail=1.0, on_start=None):
    od, idev = find_dev(out_name, 'output'), find_dev(mic_name, 'input')
    # Shared mode with eStreamOptionRaw (1). The default capture path runs
    # Windows' voice processing, whose noise suppression removes a steady sine
    # as if it were hum and gates quiet passages to exact zeros. Exclusive mode
    # on this laptop's Realtek driver delivers 65-82k frames/s for a 48k stream
    # -- repeated audio, i.e. clicks of its own. RAW bypasses the effects at
    # the right rate.
    fs_in = 48000
    ws = sd.WasapiSettings()
    ws._streaminfo.streamOption = 1
    chunks = []
    def cb(indata, frames, t, status):
        chunks.append(indata[:, 0].astype(np.float64))
    with sd.InputStream(device=idev, channels=2, samplerate=fs_in, dtype='float32',
                        callback=cb, extra_settings=ws):
        time.sleep(0.3)
        if on_start:
            on_start()
        sd.play(sig, fs_out, device=od, blocking=True,
                extra_settings=sd.WasapiSettings(auto_convert=True))
        time.sleep(tail)
    return np.concatenate(chunks), fs_in


def save_wav(path, x, fs):
    y = np.clip(x * 32767, -32768, 32767).astype(np.int16)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(fs)
        w.writeframes(y.tobytes())


def load_wav(path):
    with wave.open(path, 'rb') as w:
        fs = w.getframerate()
        y = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16)
    return y.astype(np.float64) / 32768, fs


def analyze(x, fs, freq, plot=None):
    from scipy import signal
    # The tone as recorded: the node's I2S clock and the mic's are not the
    # PC's, and gaps shift the apparent pitch, so do not assume 997.
    F = np.abs(np.fft.rfft(x * np.hanning(len(x)), n=1 << 22))
    fr = np.fft.rfftfreq(1 << 22, 1 / fs)
    band = (fr > freq * 0.97) & (fr < freq * 1.03)
    f0 = fr[band][np.argmax(F[band])]

    bp = signal.butter(4, [f0 * 0.8, f0 * 1.2], 'bandpass', fs=fs, output='sos')
    env = np.abs(signal.hilbert(signal.sosfiltfilt(bp, x)))
    y = x.copy()
    for k in range(1, int((fs / 2 - 200) // f0) + 1):
        b, a = signal.iirnotch(k * f0, 30, fs)
        y = signal.filtfilt(b, a, y)
    y = signal.sosfiltfilt(signal.butter(4, 1500, 'highpass', fs=fs, output='sos'), y)

    ms = fs // 1000
    n = len(x) // ms
    env_ms = env[:n * ms].reshape(n, ms).mean(1)
    res_ms = np.sqrt((y[:n * ms].reshape(n, ms) ** 2).mean(1))

    # The tone is the longest stretch within 10 dB of the 75th percentile of a
    # 50 ms-smoothed envelope: robust to a knock on the desk, which is loud but
    # short, and to the dips themselves, which are short too.
    sm = np.convolve(env_ms, np.ones(50) / 50, mode='same')
    on = sm > np.percentile(sm, 75) * 10 ** (-10 / 20)
    runs = np.flatnonzero(np.diff(np.r_[0, on.astype(int), 0])).reshape(-1, 2)
    if not len(runs) or (runs[:, 1] - runs[:, 0]).max() < 1000:
        print('tone not found in the recording -- is the speaker playing, and loud enough?')
        return
    i0, i1 = runs[np.argmax(runs[:, 1] - runs[:, 0])]
    # 200 ms in from each end, so the fades are not counted as dips.
    seg_t = slice(i0 + 200, i1 - 200)

    def events(mask):
        st = np.flatnonzero(mask & ~np.r_[False, mask[:-1]])
        en = np.flatnonzero(mask & ~np.r_[mask[1:], False])
        return st, en - st + 1

    level = 20 * np.log10(np.median(env_ms[seg_t]) + 1e-12)
    print(f'tone {f0:.2f} Hz, {(i1 - i0) / 1000:.2f} s long, {level:.1f} dBFS at the mic')
    if level < -55:
        print('  (too quiet to trust the dip count: raise --level or the volume)')
    for name, seg in (('lead-in', slice(200, max(i0 - 500, 300))),
                      ('tone', seg_t),
                      ('silence', slice(i1 + 500, n - 800))):
        v = res_ms[seg]
        if len(v) < 100:
            continue
        floor = np.median(v)
        st, _ = events(v > floor * 10 ** (15 / 20))
        dur = len(v) / 1000
        print(f'  {name:8s} {dur:5.1f} s  clicks {len(st):4d} ({len(st) / dur:6.2f}/s)'
              f'  residual floor {20 * np.log10(floor + 1e-12):6.1f} dBFS')
    v = env_ms[seg_t]
    st, du = events(v < np.median(v) * 10 ** (-6 / 20))
    dur = len(v) / 1000
    print(f'  dips >6 dB: {len(st)} ({len(st) / dur:.2f}/s)' +
          (f', lasting median {np.median(du):.0f} ms, max {du.max()} ms' if len(du) else ''))
    if len(st) > 3:
        gaps = np.diff(st)
        print(f'  dip spacing ms: median {np.median(gaps):.0f}, '
              f'p10 {np.percentile(gaps, 10):.0f}, p90 {np.percentile(gaps, 90):.0f}')
    if plot:
        import matplotlib; matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        c = (i0 + 1200) * ms                   # 1 s into the steady part
        w = int(0.25 * fs)
        tt = np.arange(w) / fs * 1000
        fig, ax = plt.subplots(3, 1, figsize=(14, 8), sharex=True)
        ax[0].plot(tt, x[c:c + w], lw=0.5); ax[0].set_ylabel('mic')
        ax[1].plot(tt, env[c:c + w], lw=0.8); ax[1].set_ylabel('tone envelope')
        ax[2].plot(tt, y[c:c + w], lw=0.5); ax[2].set_ylabel('residual >1.5 kHz')
        ax[2].set_xlabel('ms')
        fig.suptitle(f'{os.path.basename(plot)}: 250 ms, 1 s into the tone')
        fig.tight_layout(); fig.savefig(plot + '.png', dpi=90)


CLIENT_KEYS = ('rx', 'lost', 'rec', 'ovf', 'und', 'dup', 'rsy', 'ins', 'drp')


def client_counters(ser, sp):
    """The client's [BENCH] telemetry line (`r`), as a dict, or None."""
    sp.reset_input_buffer()
    for line in ser.talk(sp, 'r', 0.4, show=False):
        if line.startswith('[BENCH] ms='):
            return dict(kv.split('=', 1) for kv in line.split()[1:] if '=' in kv)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--analyze', metavar='WAV', help='analyse an earlier recording instead')
    ap.add_argument('--device', default='SonoLoco', help='output endpoint name (substring)')
    ap.add_argument('--mic', default='Microphone', help='input endpoint name (substring)')
    ap.add_argument('--serial', metavar='COMn', help="the node's port, for its A2DP window")
    ap.add_argument('--client', metavar='COMn', action='append', default=[],
                    help="a mesh client's port: its counters before and after, for the "
                         "mesh path. Repeat it for every client")
    ap.add_argument('--pre', default='', help='serial commands to send first, e.g. f')
    ap.add_argument('--at', action='append', default=[], metavar='SEC:CMD',
                    help='send CMD over serial SEC seconds into playback, e.g. 3.5:j')
    ap.add_argument('--label', default='', help='folded into the file name')
    ap.add_argument('--lead', type=float, default=1.5)
    ap.add_argument('--tone', type=float, default=6.0)
    ap.add_argument('--silence', type=float, default=3.0)
    ap.add_argument('--freq', type=float, default=997.0)
    ap.add_argument('--level', type=float, default=-20.0, help='dBFS, before the node volume')
    a = ap.parse_args()
    if a.analyze:
        x, fs = load_wav(a.analyze)
        analyze(x, fs, a.freq, plot=a.analyze[:-4])
        return

    fs_out = 44100
    sig = make_signal(fs_out, a.lead, a.tone, a.silence, a.freq, a.level)
    sp = None
    if a.serial:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ser
        sp = ser.open_port(a.serial)
        if a.pre:
            ser.talk(sp, a.pre, 0.5)
    clients = []                                # (port, serial, counters before)
    if a.client:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import ser
        for port in a.client:
            cp = ser.open_port(port)
            clients.append((port, cp, client_counters(ser, cp)))
            ser.talk(cp, 'l', 0.3, show=False)  # reset the loss-run histogram
    print(f'{datetime.datetime.now():%H:%M:%S} playing {len(sig) / fs_out:.1f} s '
          f'to {a.device!r} at {a.level:.0f} dBFS')
    # The board's window is reset as late as possible, right as playback opens;
    # --at commands are timed from the same moment.
    def on_start():
        sp.write(b'a')
        for spec in a.at:
            sec, cmd = spec.split(':', 1)
            threading.Timer(float(sec), sp.write, args=(cmd.encode(),)).start()
    x, fs = record(sig, fs_out, a.device, a.mic, on_start=on_start if sp else None)
    if sp:
        sp.reset_input_buffer()
        ser.talk(sp, 'a', 0.6)
        sp.close()
    for port, cp, c0 in clients:
        c1 = client_counters(ser, cp)
        runs = [l for l in ser.talk(cp, 'l', 0.3, show=False) if l.startswith('[LOSS]')]
        cp.close()
        if c0 and c1:
            print(f'client {port} ' + ' '.join(f'{k}=+{int(c1.get(k, 0)) - int(c0.get(k, 0))}' for k in CLIENT_KEYS)
                  + f" jit={c1['jit']} mode={c1['mode']}")
            if runs:
                print(f'client {port} ' + runs[0].split(' runs=')[-1].join(['lost runs of 1..7,8+ = ', '']))
        else:
            print(f'client {port}: no [BENCH] line -- not a client, or not answering')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    base = os.path.join(ROOT, 'logs', f'listen-{stamp}' + (f'-{a.label}' if a.label else ''))
    os.makedirs(os.path.dirname(base), exist_ok=True)
    save_wav(base + '.wav', x, fs)
    print(f'saved {os.path.relpath(base, ROOT)}.wav, mic peak {np.abs(x).max():.2f}'
          + ('  (clipped: lower the level)' if np.abs(x).max() >= 0.99 else ''))
    if not np.any(x):
        # A real room is never exactly zero: this is the input muted in Windows
        # (2026-10-01), not a speaker too quiet or too far away.
        sys.exit('the microphone recorded exact zeros -- it is muted in Windows; unmute it and rerun')
    analyze(x, fs, a.freq, plot=base)


if __name__ == '__main__':
    main()
