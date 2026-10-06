#ifndef TONES_H
#define TONES_H

/**
 * The notification sounds: water drops through an echo.
 *
 * A drop is a sine that starts a few semitones under its note and glides up
 * into it -- the plink of a drop landing in water -- then dies away
 * exponentially. On its attack it can carry a glassy shimmer: two-operator FM
 * whose index decays faster than the note, the sound of DX7 bells and
 * polyphonic ringtones.
 *
 * The echo is not a delay line. Each drop is played again, later and quieter,
 * as one more voice, and each repeat keeps less of the shimmer, the way a tape
 * echo loses its top end. A delay line of 210 ms is 18 KB, on a streaming
 * server whose largest free block was 13 KB (2026-10-03); the voices are under
 * a kilobyte, on the stack, and the sound is the same.
 *
 * Per sample it is integer arithmetic only: a quarter-wave sine table in
 * flash, a 32-bit phase per voice, multiplies and shifts. The C3 has no FPU,
 * and a sinf() a sample is what made its beeps stutter (docs/gotchas.md).
 * begin() works in double, a few dozen operations a sound, so that every
 * coefficient rounds exactly as it did in the browser preview the sounds were
 * tuned in by ear (2026-10-06): that preview runs this same arithmetic, and
 * test_tones holds the two to the same samples.
 *
 * Mono: the caller writes each sample to both channels. A ping-pong echo
 * would lose half its repeats on a MAX98357A, which plays one channel.
 *
 * Pure logic, no Arduino or ESP-IDF; tested in test/test_tones.
 */

#include <stdint.h>

class DropSound {
public:
    /**
     * How every drop of a sound is made: config.h's DROP_* values. begin()
     * clamps each into the range the integer arithmetic has room for.
     */
    struct Timbre {
        double glideSemitones;  ///< a drop starts this far under its note (0..24)
        double glideMs;         ///< and closes on it with this time constant (5..)
        double decayMs;         ///< the note's time constant, before Note::decayMul
        double glass;           ///< FM index at the attack, radians; 0 is a pure sine (0..3)
        double glassRatio;      ///< modulator : carrier (0..4)
        double glassMs;         ///< the shimmer's time constant (5..)
        double echoGlassKeep;   ///< the fraction of shimmer each repeat keeps (0..1)
        double echoMs;          ///< from a drop to its first repeat, and between repeats
        double echoLevel;       ///< each repeat's gain, relative to the one before (0..1)
        int    echoRepeats;     ///< at most this many; one under -54 dB is not played
    };

    /** One drop. */
    struct Note {
        double atMs;            ///< onset, from the start of the sound
        int    midi;            ///< the note it settles on: 69 is A4, 440 Hz. Under 4 kHz
        double gain;            ///< of the peak amplitude, 0..1
        double decayMul;        ///< on Timbre::decayMs, for this drop
    };

    /** Drops and their repeats: three drops with three repeats each are 12. */
    static const int MAX_VOICES = 16;

    /** Set up to play `notes`. Voices past MAX_VOICES are left out. */
    void begin(const Note *notes, int count, const Timbre &t, uint32_t sampleRate);

    /**
     * Render up to `frames` mono samples. A drop at gain 1 peaks at `amp`;
     * where echoes overlap a drop they add a few percent. amp 0 renders
     * silence for exactly as long, which is what a muted node plays.
     * Returns the samples written: fewer than asked when the sound ends, then 0.
     */
    int render(int16_t *out, int frames, int32_t amp);

    /** Voices set up by begin(). */
    int voices() const { return count_; }

private:
    struct Voice {
        uint32_t start;      // first sample
        uint32_t age;        // samples since start
        uint32_t phase;      // carrier, a full turn is 2^32
        uint32_t modPhase;   // shimmer's modulator
        int32_t  inc;        // carrier step, gliding towards target
        int32_t  target;     // the note's step
        int32_t  env;        // Q24: 1 << 24 at the end of the attack
        int32_t  kDecay;     // Q20 one-pole coefficient
        int32_t  glassQ;     // FM index, as (radians / 2 pi) << 25
        int32_t  gain;       // Q15
        int32_t  endEnv;     // the env under which this voice is below -54 dB
        bool     done;
    };

    Voice    voice_[MAX_VOICES];
    int      count_    = 0;
    uint32_t n_        = 0;    // samples rendered
    uint32_t attack_   = 0;    // 1 ms, linear
    int32_t  attackStep_ = 0;
    int32_t  kGlide_   = 0;
    int32_t  kGlass_   = 0;
    int32_t  ratioQ8_  = 0;
};

#endif  // TONES_H
