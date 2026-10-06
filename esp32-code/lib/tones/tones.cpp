#include "tones.h"

#include <math.h>

// A quarter of a sine, round(32767 * sin(2 pi i / 1024)) for i = 0..256, in
// flash: no RAM on any node, nothing to compute at boot. Mirrored, it is the
// browser preview's 1,024-entry table to the last sample.
static const int16_t QUARTER_SINE[257] = {
        0,   201,   402,   603,   804,  1005,  1206,  1407,  1608,  1809,  2009,  2210,
     2410,  2611,  2811,  3012,  3212,  3412,  3612,  3811,  4011,  4210,  4410,  4609,
     4808,  5007,  5205,  5404,  5602,  5800,  5998,  6195,  6393,  6590,  6786,  6983,
     7179,  7375,  7571,  7767,  7962,  8157,  8351,  8545,  8739,  8933,  9126,  9319,
     9512,  9704,  9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269, 15446, 15623, 15800, 15976,
    16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000,
    20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311, 23452, 23592,
    23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
    26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050,
    31113, 31176, 31237, 31297, 31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250,
    32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752,
    32757, 32761, 32765, 32766, 32767,
};

/** sin(phase), Q15, for a phase where a full turn is 2^32. */
static inline int32_t sine(uint32_t phase) {
    const uint32_t i = phase >> 22;   // 0..1023
    const uint32_t q = i & 255;
    switch (i >> 8) {
        case 0:  return  QUARTER_SINE[q];
        case 1:  return  QUARTER_SINE[256 - q];
        case 2:  return -QUARTER_SINE[q];
        default: return -QUARTER_SINE[256 - q];
    }
}

// A voice stops below -54 dB of full scale: 2^24 / 500. At the loudest node's
// TONE_AMPLITUDE that is 8 counts of 32,767.
static const int32_t END_ENV = 33554;

static const double TWO_PI = 6.283185307179586;   // 2 * M_PI, which strict builds leave undefined

static double clampd(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }

/** The Q20 one-pole coefficient for a time constant: per sample, move this fraction of the way. */
static int32_t coefQ20(double ms, uint32_t rate) {
    return (int32_t)llround(1048576.0 * (1.0 - exp(-1000.0 / (ms * rate))));
}

void DropSound::begin(const Note *notes, int count, const Timbre &t, uint32_t sampleRate) {
    // The clamps are what keep every product below in an int32: the glide's
    // at 5 ms and 4 kHz, the shimmer's at an index of 3 (95% of 2^31).
    const double glideSt = clampd(t.glideSemitones, 0, 24);
    const double glass   = clampd(t.glass, 0, 3);
    const double keep    = clampd(t.echoGlassKeep, 0, 1);
    const double level   = clampd(t.echoLevel, 0, 1);
    const int    repeats = t.echoRepeats < 0 ? 0 : t.echoRepeats > MAX_VOICES ? MAX_VOICES : t.echoRepeats;

    attack_     = sampleRate >= 1000 ? sampleRate / 1000 : 1;
    attackStep_ = (int32_t)((1u << 24) / attack_);
    kGlide_     = coefQ20(clampd(t.glideMs, 5, 1e6), sampleRate);
    kGlass_     = coefQ20(clampd(t.glassMs, 5, 1e6), sampleRate);
    ratioQ8_    = (int32_t)llround(clampd(t.glassRatio, 0, 4) * 256);
    n_          = 0;
    count_      = 0;

    const double from  = pow(2.0, -glideSt / 12);
    const long long glass0 = llround(glass / TWO_PI * 131072);

    for (int i = 0; i < count; i++) {
        const Note  &nt     = notes[i];
        const int    midi   = nt.midi > 107 ? 107 : nt.midi;   // B7, 3,951 Hz
        const double hz     = 440 * pow(2.0, (midi - 69) / 12.0);
        const int32_t target = (int32_t)llround(hz * 4294967296.0 / sampleRate);
        const int32_t kDecay = coefQ20(clampd(t.decayMs * nt.decayMul, 5, 1e6), sampleRate);
        const uint32_t start0 = (uint32_t)llround(nt.atMs * sampleRate / 1000);

        for (int k = 0; k <= repeats && count_ < MAX_VOICES; k++) {
            const int32_t gain = (int32_t)llround(32767 * clampd(nt.gain, 0, 1) * pow(level, k));
            if (gain < 66) break;                        // under -54 dB, and so is every later repeat
            Voice &v   = voice_[count_++];
            v.start    = start0 + (uint32_t)llround(k * t.echoMs * sampleRate / 1000);
            v.age      = 0;
            v.phase    = 0;
            v.modPhase = 0;
            v.target   = target;
            v.inc      = (int32_t)llround(target * from);
            v.env      = 0;
            v.kDecay   = kDecay;
            v.glassQ   = (int32_t)llround(glass0 * pow(keep, k)) * 256;
            v.gain     = gain;
            const int64_t end = (int64_t)END_ENV * 32767 / gain;
            v.endEnv   = end > (1 << 24) ? (1 << 24) : (int32_t)end;
            v.done     = false;
        }
    }
}

int DropSound::render(int16_t *out, int frames, int32_t amp) {
    int i = 0;
    for (; i < frames; i++, n_++) {
        int32_t sum  = 0;
        int     live = 0;
        for (int j = 0; j < count_; j++) {
            Voice &v = voice_[j];
            if (v.done) continue;
            live++;
            if (n_ < v.start) continue;

            // Glide up into the note, a one-pole on the phase step.
            v.inc += (((v.target - v.inc) >> 10) * kGlide_) >> 10;

            // The shimmer: a modulator at ratio x the carrier, bending its phase.
            uint32_t bend = 0;
            if (v.glassQ > 0) {
                v.modPhase += (uint32_t)((v.inc >> 8) * ratioQ8_);
                bend        = (uint32_t)(sine(v.modPhase) * (v.glassQ >> 8));
                v.glassQ   -= ((v.glassQ >> 8) * kGlass_) >> 12;
            }
            v.phase += (uint32_t)v.inc;
            const int32_t s = sine(v.phase + bend);

            if (v.age < attack_)       v.env = (int32_t)v.age * attackStep_;
            else if (v.age == attack_) v.env = 1 << 24;
            else                       v.env -= ((v.env >> 8) * v.kDecay) >> 12;
            v.age++;

            sum += (((s * (v.env >> 9)) >> 15) * v.gain) >> 15;
            if (v.age > attack_ && v.env < v.endEnv) v.done = true;
        }
        if (live == 0) break;

        int32_t o = (int32_t)(((int64_t)sum * amp) >> 15);
        if (o > 32767)  o = 32767;
        if (o < -32768) o = -32768;
        out[i] = (int16_t)o;
    }
    return i;
}
