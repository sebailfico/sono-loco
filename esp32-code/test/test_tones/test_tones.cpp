/**
 * Tests for the notification sounds (lib/tones).
 *
 * Run on a board:  pio test -e esp32c3 -f test_tones   (or -e native, given a
 * host compiler). The C3 is the board that matters: it has no FPU, and its
 * beeps once took 1,410 ms to play 330 ms of sound. The last test times it.
 *
 * The sounds were tuned by ear in a browser preview that runs the same integer
 * arithmetic in JavaScript. The first test holds the firmware to that preview's
 * samples, every one of them: a change here that alters a single sample is a
 * different sound from the one that was chosen, and should be heard first.
 * The rest pin what the sounds mean, which survives a retune: each drop lands
 * on its note, rising or falling, a muted node plays silence for as long, and
 * nothing depends on how the caller chunks the output.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <unity.h>

#include "tones.h"

static const uint32_t RATE = 44100;
static const int32_t  AMP  = 8000;    // the preview's level

// The tuned sounds, kept in step with config.h's DROP_* values and the three
// sounds in main.cpp.
static const DropSound::Timbre TUNED = {3, 14, 115, 0.5, 3.5, 20, 0.5, 210, 0.13, 3};
static const int LOW = 82, MID = 85, HIGH = 90;   // A#5, C#6, F#6

static const DropSound::Note LISTENING[] = {{0, MID, 0.8, 1}, {160, MID, 0.8, 1.4}};
static const DropSound::Note JOINED[]    = {{0, LOW, 0.8, 1}, {160, MID, 0.8, 1}, {320, HIGH, 0.8, 1.8}};
static const DropSound::Note NOTHING[]   = {{0, HIGH, 0.8, 1}, {160, MID, 0.8, 1}, {320, LOW, 0.8, 1.8}};

struct Sound {
    const char           *name;
    const DropSound::Note *notes;
    int                   count;
    uint32_t              frames;    // from the preview
    uint32_t              hash;      // FNV-1a over the samples, little-endian
};

static const Sound SOUNDS[] = {
    {"listening",     LISTENING, 2, 50201, 0x888FEA5Fu},
    {"joined",        JOINED,    3, 69912, 0x3B15ADDBu},
    {"nothing heard", NOTHING,   3, 69912, 0x9198C74Cu},
};

void setUp(void) {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// What a rendered sound is, measured as it streams: no buffer the size of a
// sound, which would not fit beside a classic ESP32's other statics.
// ---------------------------------------------------------------------------

struct Probe {
    uint32_t frames = 0;
    uint32_t hash   = 2166136261u;
    int32_t  peak   = 0;
    int16_t  tail[441] = {0};        // the last 10 ms

    // Upward zero crossings inside [from, to), interpolated between samples.
    uint32_t from = 0, to = 0;
    int16_t  prev = 0;
    double   firstUp = -1, lastUp = -1;
    int      ups = 0;

    void feed(int16_t s) {
        const uint16_t u = (uint16_t)s;
        hash = (hash ^ (u & 0xFF)) * 16777619u;
        hash = (hash ^ (u >> 8)) * 16777619u;
        const int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
        tail[frames % 441] = s;
        if (frames > from && frames < to && prev < 0 && s >= 0) {
            const double t = (frames - 1) + (double)(-prev) / (s - prev);
            if (firstUp < 0) firstUp = t;
            lastUp = t;
            ups++;
        }
        prev = s;
        frames++;
    }

    double hz() const { return (ups - 1) / ((lastUp - firstUp) / RATE); }

    int32_t tailPeak() const {
        int32_t m = 0;
        for (int i = 0; i < 441; i++) {
            const int32_t a = tail[i] < 0 ? -tail[i] : tail[i];
            if (a > m) m = a;
        }
        return m;
    }
};

static uint32_t msToFrames(double ms) { return (uint32_t)lround(ms * RATE / 1000); }

static Probe play(const DropSound::Note *notes, int count, const DropSound::Timbre &t, int32_t amp,
                  int chunk = 512, double fromMs = 0, double toMs = 0) {
    static DropSound sound;
    static int16_t   buf[4096];
    Probe p;
    p.from = msToFrames(fromMs);
    p.to   = msToFrames(toMs);
    sound.begin(notes, count, t, RATE);
    int n;
    while ((n = sound.render(buf, chunk, amp)) > 0)
        for (int i = 0; i < n; i++) p.feed(buf[i]);
    return p;
}

static double noteHz(int midi) { return 440 * pow(2.0, (midi - 69) / 12.0); }

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

/** Every sample as the preview made it, so the board plays what was chosen by ear. */
void test_sounds_are_the_ones_tuned_in_the_preview(void) {
    for (const Sound &s : SOUNDS) {
        const Probe p = play(s.notes, s.count, TUNED, AMP);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(s.frames, p.frames, s.name);
        TEST_ASSERT_EQUAL_HEX32_MESSAGE(s.hash, p.hash, s.name);
    }
}

/**
 * A muted node plays its sounds as silence, for exactly as long: what follows a
 * jingle (the schedule it invalidates) runs as it would have.
 */
void test_a_muted_node_plays_silence_for_as_long(void) {
    for (const Sound &s : SOUNDS) {
        const Probe p = play(s.notes, s.count, TUNED, 0);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(s.frames, p.frames, s.name);
        TEST_ASSERT_EQUAL_INT32_MESSAGE(0, p.peak, s.name);
    }
}

/** main.cpp writes in chunks of whatever the driver takes; the sound must not care. */
void test_chunk_size_does_not_change_the_sound(void) {
    const int chunks[] = {1, 37, 512, 4096};
    for (int c : chunks) {
        const Probe p = play(JOINED, 3, TUNED, AMP, c);
        TEST_ASSERT_EQUAL_UINT32(SOUNDS[1].frames, p.frames);
        TEST_ASSERT_EQUAL_HEX32(SOUNDS[1].hash, p.hash);
    }
}

/**
 * Each drop settles on its note, which is what makes "joined" rise and
 * "nothing heard" fall -- the only thing a person at a node with no console
 * has to go on. Measured 50-110 ms after each onset, past the glide and
 * before the next drop: within 0.5%, where a semitone is 6%.
 */
void test_each_drop_settles_on_its_note(void) {
    for (const Sound &s : SOUNDS) {
        for (int i = 0; i < s.count; i++) {
            const double at = s.notes[i].atMs;
            const Probe  p  = play(s.notes, s.count, TUNED, AMP, 512, at + 50, at + 110);
            const double want = noteHz(s.notes[i].midi);
            TEST_ASSERT_FLOAT_WITHIN_MESSAGE((float)(want * 0.005), (float)want, (float)p.hz(), s.name);
        }
    }
}

/**
 * A drop starts under its note and glides up into it. On a lone drop with no
 * shimmer, whose sidebands would add zero crossings of their own: 3 semitones
 * under, with a 14 ms time constant, averages 0.90 of the note over its first
 * 12 ms and 0.99 by 30-40 ms.
 */
void test_a_drop_glides_up_into_its_note(void) {
    DropSound::Timbre pure = TUNED;
    pure.glass       = 0;
    pure.echoRepeats = 0;
    const DropSound::Note one[] = {{0, MID, 1, 1}};
    const double want  = noteHz(MID);
    const double early = play(one, 1, pure, AMP, 512, 1, 12).hz() / want;
    const double later = play(one, 1, pure, AMP, 512, 30, 40).hz() / want;
    TEST_ASSERT_FLOAT_WITHIN(0.05f, 0.90f, (float)early);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.99f, (float)later);
}

/**
 * Loud as the old beeps, which peaked at exactly TONE_AMPLITUDE: drops at 0.8
 * leave the overlapping echoes room. And each sound ends by fading out, not by
 * stopping -- the last 10 ms are under -54 dB.
 */
void test_peak_is_the_old_beeps_and_the_end_is_quiet(void) {
    for (const Sound &s : SOUNDS) {
        const Probe p = play(s.notes, s.count, TUNED, AMP);
        TEST_ASSERT_INT32_WITHIN_MESSAGE(AMP / 10, AMP, p.peak, s.name);
        TEST_ASSERT_LESS_THAN_INT32_MESSAGE(AMP / 400, p.tailPeak(), s.name);
    }
}

/**
 * A repeat under -54 dB is not played, and nor is any after it. At an echo
 * level of 0.13 a drop at 0.8 has repeats at 0.104 and 0.0135; the third,
 * 0.0018, is left out, so three drops are nine voices, not twelve.
 */
void test_inaudible_repeats_are_left_out(void) {
    DropSound sound;
    sound.begin(JOINED, 3, TUNED, RATE);
    TEST_ASSERT_EQUAL_INT(9, sound.voices());
}

/** More voices than there is room for: the extra ones are left out, nothing overruns. */
void test_voices_past_the_limit_are_left_out(void) {
    const DropSound::Note six[] = {{0, LOW, 1, 1},  {50, MID, 1, 1},  {100, HIGH, 1, 1},
                                   {150, LOW, 1, 1}, {200, MID, 1, 1}, {250, HIGH, 1, 1}};
    DropSound::Timbre loud = TUNED;
    loud.echoLevel = 0.6;    // every repeat audible: 6 drops x 4 voices asked for
    DropSound sound;
    sound.begin(six, 6, loud, RATE);
    TEST_ASSERT_EQUAL_INT(DropSound::MAX_VOICES, sound.voices());
    const Probe p = play(six, 6, loud, AMP);
    TEST_ASSERT_GREATER_THAN_UINT32(0, p.frames);
}

/** Out-of-range settings are clamped into what the int32 arithmetic has room for. */
void test_settings_out_of_range_are_clamped_not_overflowed(void) {
    DropSound::Timbre wild = {40, 1, 2, 9, 9, 1, 2, 80, 1.5, 99};
    const DropSound::Note top[] = {{0, 127, 1, 1}};
    const Probe p = play(top, 1, wild, AMP);
    TEST_ASSERT_GREATER_THAN_UINT32(0, p.frames);
    TEST_ASSERT_LESS_OR_EQUAL_INT32(32767, p.peak);   // an overflow shows as noise at full scale
    TEST_ASSERT_LESS_THAN_INT32(AMP * 2, p.peak);
}

#ifdef ARDUINO
#include <Arduino.h>

/**
 * Fast enough to play as it is rendered, with room to spare, on the board it
 * runs on. The C3 once computed its beeps at a sixth of the speed they play,
 * and the DMA filled the gaps with silence. Prints the figure.
 */
void test_rendering_keeps_well_ahead_of_playing(void) {
    static DropSound sound;
    static int16_t   buf[512];
    sound.begin(JOINED, 3, TUNED, RATE);
    uint32_t frames = 0;
    const uint32_t t0 = micros();
    int n;
    while ((n = sound.render(buf, 512, AMP)) > 0) frames += n;
    const uint32_t us     = micros() - t0;
    const uint32_t played = (uint32_t)((uint64_t)frames * 1000000 / RATE);
    char msg[96];
    snprintf(msg, sizeof(msg), "joined: %lu us to render %lu us of sound (%lu%%)",
             (unsigned long)us, (unsigned long)played, (unsigned long)(100 * us / played));
    TEST_MESSAGE(msg);
    TEST_ASSERT_LESS_THAN_UINT32(played / 4, us);
}
#endif

int runAllTests(void) {
    UNITY_BEGIN();

    RUN_TEST(test_sounds_are_the_ones_tuned_in_the_preview);
    RUN_TEST(test_a_muted_node_plays_silence_for_as_long);
    RUN_TEST(test_chunk_size_does_not_change_the_sound);
    RUN_TEST(test_each_drop_settles_on_its_note);
    RUN_TEST(test_a_drop_glides_up_into_its_note);
    RUN_TEST(test_peak_is_the_old_beeps_and_the_end_is_quiet);
    RUN_TEST(test_inaudible_repeats_are_left_out);
    RUN_TEST(test_voices_past_the_limit_are_left_out);
    RUN_TEST(test_settings_out_of_range_are_clamped_not_overflowed);
#ifdef ARDUINO
    RUN_TEST(test_rendering_keeps_well_ahead_of_playing);
#endif

    return UNITY_END();
}

#ifdef ARDUINO

// Same tests on the board, as the other suites: there is no host compiler on
// this machine yet.
void setup() {
    delay(2000);   // let the host's serial monitor attach before output starts
    runAllTests();
}

void loop() {}

#else

int main(void) { return runAllTests(); }

#endif
