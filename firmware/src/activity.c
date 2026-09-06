/*
 * activity.c -- non-blocking visual activity indicator for the onboard LED.
 */

#include "activity.h"

#include "pico/stdlib.h"

#define ACTIVITY_MIN_VISIBLE_MS 150
#define ACTIVITY_TIMER_MS       10

static absolute_time_t activity_off_at;
static bool activity_on;

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

    static struct repeating_timer timer;
    add_repeating_timer_ms(ACTIVITY_TIMER_MS, activity_timer_callback, NULL, &timer);
}

void activity_begin(void) {
    gpio_put(PICO_DEFAULT_LED_PIN, 1);
    activity_on = true;
    activity_off_at = make_timeout_time_ms(ACTIVITY_MIN_VISIBLE_MS);
}
