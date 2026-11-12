/**
 * Host-side tests for the button (lib/button).
 *
 * Run with:  pio test -e native -f test_button
 *
 * The edges are the whole of it: a hold that lands in the wrong range sends a
 * speaker on a wall into the wrong mode, with nobody at a console to see why.
 */

#include <unity.h>

#include "button.h"

void setUp(void) {}
void tearDown(void) {}

// The ranges D19 settled on: under a second, 3 to 9, 10 to 15.
static const ButtonTimes T = {50, 1000, 3000, 9000, 10000, 15000};

void test_a_bounce_does_nothing(void) {
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(0, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(49, T));
    TEST_ASSERT_EQUAL(BUTTON_BLUETOOTH, buttonAction(50, T));
}

void test_each_range_starts_where_it_says_and_ends_before_the_next_number(void) {
    TEST_ASSERT_EQUAL(BUTTON_BLUETOOTH, buttonAction(999, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(1000, T));
    TEST_ASSERT_EQUAL(BUTTON_PAIR, buttonAction(3000, T));
    TEST_ASSERT_EQUAL(BUTTON_PAIR, buttonAction(8999, T));
    TEST_ASSERT_EQUAL(BUTTON_CONFIG, buttonAction(10000, T));
    TEST_ASSERT_EQUAL(BUTTON_CONFIG, buttonAction(14999, T));
}

/** The gaps are margin for a hand counting seconds; past the last range is a way out. */
void test_the_gaps_and_a_hold_past_the_end_do_nothing(void) {
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(2000, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(2999, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(9500, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(15000, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonAction(600000, T));
}

void test_entering_a_range_is_said_once(void) {
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonEntered(0, 2990, T));
    TEST_ASSERT_EQUAL(BUTTON_PAIR, buttonEntered(2990, 3005, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonEntered(3005, 3020, T));
    TEST_ASSERT_EQUAL(BUTTON_CONFIG, buttonEntered(9990, 10001, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonEntered(10001, 10020, T));
}

/** loop() stalled for a second and a half by a tone: the range it is now in, not one it skipped. */
void test_a_reading_that_skips_a_range_names_the_one_it_landed_in(void) {
    TEST_ASSERT_EQUAL(BUTTON_PAIR, buttonEntered(500, 4000, T));
    TEST_ASSERT_EQUAL(BUTTON_CONFIG, buttonEntered(2500, 11000, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonEntered(8000, 9500, T));
    TEST_ASSERT_EQUAL(BUTTON_NONE, buttonEntered(9000, 16000, T));
}

static int runAllTests(void) {
    UNITY_BEGIN();
    RUN_TEST(test_a_bounce_does_nothing);
    RUN_TEST(test_each_range_starts_where_it_says_and_ends_before_the_next_number);
    RUN_TEST(test_the_gaps_and_a_hold_past_the_end_do_nothing);
    RUN_TEST(test_entering_a_range_is_said_once);
    RUN_TEST(test_a_reading_that_skips_a_range_names_the_one_it_landed_in);
    return UNITY_END();
}

#ifdef ARDUINO

#include <Arduino.h>

void setup() {
    delay(2000);   // let the host's serial monitor attach before output starts
    runAllTests();
}

void loop() {}

#else

int main(int, char **) {
    return runAllTests();
}

#endif
