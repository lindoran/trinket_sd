/*
 * activity.c -- non-blocking visual activity indicator for the onboard LED.
 */

#include "activity.h"

#include "pico/stdlib.h"
#include "debug.h"

#define ACTIVITY_MIN_VISIBLE_MS 150
#define ACTIVITY_TIMER_MS       10

static absolute_time_t activity_off_at;
static bool activity_on;
static bool activity_debug_enabled;
static bool activity_timer_failed;
static bool activity_timer_failure_reported;

static bool activity_timer_callback(struct repeating_timer *timer) {
    (void)timer;

    if (activity_on && absolute_time_diff_us(get_absolute_time(), activity_off_at) <= 0) {
        gpio_put(PICO_DEFAULT_LED_PIN, 0);
        activity_on = false;
    }

    return true;
}

void activity_init(void) {
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    gpio_put(PICO_DEFAULT_LED_PIN, 0);
    activity_on = false;
    activity_timer_failed = false;
    activity_timer_failure_reported = false;

    static struct repeating_timer timer;
    if (!add_repeating_timer_ms(ACTIVITY_TIMER_MS, activity_timer_callback, NULL, &timer)) {
        activity_timer_failed = true;
        if (activity_debug_enabled) {
            debug_printf("activity: failed to schedule LED timer\n");
            activity_timer_failure_reported = true;
        }
    }
}

void activity_begin(void) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    activity_on = true;
    activity_off_at = make_timeout_time_ms(ACTIVITY_MIN_VISIBLE_MS);
}

void activity_set_debug(bool enabled) {
    activity_debug_enabled = enabled;
    if (enabled && activity_timer_failed && !activity_timer_failure_reported) {
        debug_printf("activity: failed to schedule LED timer\n");
        activity_timer_failure_reported = true;
    }
}
