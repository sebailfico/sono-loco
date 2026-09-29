"""Hear what a mesh client would play, before any of it is firmware.

    python tools/codec/abtest.py song.wav [--out logs/abtest]

Takes a 44.1 kHz stereo 16-bit WAV and writes, all at 44.1 kHz stereo so a
player can switch between them without a jump in rate or level:

  <name>-0-original.wav    the input, untouched
  <name>-A-today.wav       what a client plays today: L+R folded to mono, the
                           [1 3 3 1]/8 FIR and every other sample kept
                           (22.05 kHz), in the same integer arithmetic as
                           a2dpForwardPacket() in main.cpp. Brought back to
                           44.1 kHz with a sharp interpolator, which is the
                           job the DAC's oversampling filter does on a board.
  <name>-B-adpcm.wav       44.1 kHz stereo through IMA ADPCM, 4 bits a sample,
                           the decoder restarted from a carried state at every
                           block, as each mesh packet would carry it.
  <name>-B-noise-only.wav  original minus B, at its real level: the noise
                           ADPCM adds, on its own.

It prints how each differs from the original: the signal-to-noise ratio of B,
and for both, the energy left above 8 kHz and in the stereo difference (L-R).

The ADPCM here is the reference for a firmware codec: encode() and decode()
are written the way the C will be, sample by sample in integers, and decode()
is checked against the encoder's own reconstruction on every run.
"""
import argparse, os, wave
import numpy as np

# IMA ADPCM (Intel/DVI), the standard tables.
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
        253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
        1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
        3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
        11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767]
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]

# Frames per block: two blocks of 2 x 4-bit stereo plus their carried state fit
# one 250-byte ESP-NOW frame -- the "previous block in every packet" layout.
BLOCK = 114


def _step(code, pred, idx):
    """The decoder's update, shared by both sides so they cannot disagree."""
    step = STEP[idx]
    diff = step >> 3
    if code & 4: diff += step
    if code & 2: diff += step >> 1
    if code & 1: diff += step >> 2
    pred = pred - diff if code & 8 else pred + diff
    pred = max(-32768, min(32767, pred))
    idx = max(0, min(88, idx + INDEX[code & 7]))
    return pred, idx


def encode(x):
    """One channel of int16 -> (codes, per-block (pred, idx) states, reconstruction)."""
    codes = np.zeros(len(x), np.uint8)
    states, recon = [], np.zeros(len(x), np.int16)
    pred, idx = 0, 0                       # as AdpcmStereoEncoder::reset()
    for i, s in enumerate(x.tolist()):
        if i % BLOCK == 0:
            states.append((pred, idx))     # what the packet header carries
        step = STEP[idx]
        diff = s - pred
        code = 0
        if diff < 0:
            code, diff = 8, -diff
        if diff >= step: code |= 4; diff -= step
        if diff >= step >> 1: code |= 2; diff -= step >> 1
        if diff >= step >> 2: code |= 1
        pred, idx = _step(code, pred, idx)
        codes[i], recon[i] = code, pred
    return codes, states, recon


def decode(codes, states):
    """What a client does: start each block from its carried state."""
    out = np.zeros(len(codes), np.int16)
    pred = idx = 0
    for i, c in enumerate(codes.tolist()):
        if i % BLOCK == 0:
            pred, idx = states[i // BLOCK]
        pred, idx = _step(c, pred, idx)
        out[i] = pred
    return out


def today(stereo):
    """a2dpForwardPacket(): fold, [1 3 3 1]/8, keep every other sample."""
    l, r = stereo[:, 0].astype(np.int32), stereo[:, 1].astype(np.int32)
    mono = (l + r) >> 1
    h = np.zeros(len(mono) + 3, np.int32)
    h[3:] = mono
    # decimHist[0] is the newest sample; the output is taken on every second one.
    f = (h[3:] + 3 * h[2:-1] + 3 * h[1:-2] + h[:-3]) >> 3
    return np.clip(f[1::2], -32768, 32767).astype(np.int16)


def read_wav(path):
    with wave.open(path, 'rb') as w:
        assert w.getsampwidth() == 2 and w.getnchannels() == 2 and w.getframerate() == 44100, \
            'needs 44.1 kHz stereo 16-bit'
        return np.frombuffer(w.readframes(w.getnframes()), np.int16).reshape(-1, 2).copy()


def write_wav(path, stereo):
    with wave.open(path, 'wb') as w:
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(44100)
        w.writeframes(np.ascontiguousarray(stereo, np.int16).tobytes())


def band_db(stereo, lo_hz):
    """Energy above lo_hz, dB relative to full scale, both channels."""
    x = stereo.astype(np.float64) / 32768
    F = np.abs(np.fft.rfft(x, axis=0)) ** 2
    f = np.fft.rfftfreq(len(x), 1 / 44100)
    return 10 * np.log10(F[f >= lo_hz].sum() / len(x) ** 2 + 1e-20)


def side_db(stereo):
    s = (stereo[:, 0].astype(np.float64) - stereo[:, 1]) / 65536
    return 10 * np.log10((s ** 2).mean() + 1e-20)


def main():
    from scipy.signal import resample_poly
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('wav', nargs='+')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(__file__), '..', '..', 'logs', 'abtest'))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    for path in a.wav:
        name = os.path.splitext(os.path.basename(path))[0]
        x = read_wav(path)

        m = today(x)
        up = np.clip(np.round(resample_poly(m.astype(np.float64), 2, 1)), -32768, 32767)
        A = np.repeat(up[:len(x), None], 2, axis=1).astype(np.int16)

        B = np.zeros_like(x)
        for ch in range(2):
            codes, states, recon = encode(x[:, ch])
            B[:, ch] = decode(codes, states)
            assert np.array_equal(B[:, ch], recon), 'decoder disagrees with encoder'

        noise = x.astype(np.int32) - B
        snr = 10 * np.log10((x.astype(np.float64) ** 2).sum() / (noise.astype(np.float64) ** 2).sum())
        for tag, y in (('0-original', x), ('A-today', A), ('B-adpcm', B),
                       ('B-noise-only', np.clip(noise, -32768, 32767))):
            write_wav(os.path.join(a.out, f'{name}-{tag}.wav'), y)

        print(f'{name}: {len(x) / 44100:.1f} s')
        print(f'  ADPCM signal-to-noise {snr:.1f} dB over the whole excerpt')
        print(f'  {"":10s} {"original":>9s} {"A today":>9s} {"B adpcm":>9s}')
        for lo in (8000, 12000):
            print(f'  >{lo // 1000:2d} kHz dB {band_db(x, lo):9.1f} {band_db(A, lo):9.1f} {band_db(B, lo):9.1f}')
        print(f'  L-R dB    {side_db(x):9.1f} {side_db(A):9.1f} {side_db(B):9.1f}')
    print(f'written to {os.path.abspath(a.out)}')


if __name__ == '__main__':
    main()
