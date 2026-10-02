/*
 * Unity's output, the same as PlatformIO generates in every way but one: a run
 * does not end on Serial.end().
 *
 * On a C3 or an S3, `Serial` is the chip's own USB port, not a bridge chip in
 * front of it. Ending it took a C3 off USB the moment its tests had passed,
 * and on every power-up after -- the test image running again -- until it was
 * held in download mode with BOOT and reflashed (2026-10-02). A classic board
 * never shows it: its USB is a CH340 that Serial.end() cannot reach.
 *
 * Compiled into every suite from the root of test/, on the boards and on the
 * host alike.
 */

#include <unity_config.h>

#ifdef ARDUINO

#include <Arduino.h>

void unityOutputStart(unsigned long baudrate) { Serial.begin(baudrate); }
void unityOutputChar(unsigned int c) { Serial.write(c); }
void unityOutputFlush(void) { Serial.flush(); }
void unityOutputComplete(void) { Serial.flush(); }

#else

#include <stdio.h>

void unityOutputStart(unsigned long baudrate) { (void)baudrate; }
void unityOutputChar(unsigned int c) { putchar(c); }
void unityOutputFlush(void) { fflush(stdout); }
void unityOutputComplete(void) {}

#endif

// Every suite defines setUp and tearDown; these only stand in for one that
// does not, as the generated configuration's did.
extern "C" {
__attribute__((weak)) void setUp(void) {}
__attribute__((weak)) void tearDown(void) {}
__attribute__((weak)) void suiteSetUp(void) {}
__attribute__((weak)) int  suiteTearDown(int num_failures) { return num_failures; }
}
