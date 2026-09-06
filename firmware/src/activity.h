/*
 * activity.h -- non-blocking visual activity indicator for the onboard LED.
 */

#ifndef TRINKET_ACTIVITY_H
#define TRINKET_ACTIVITY_H

#include <stdbool.h>

/* Initializes the LED and its timer. */
void activity_init(void);

/* Marks the beginning of an operation and starts/extends the visible pulse. */
void activity_begin(void);

/* Enables/disables activity indicator diagnostics on the USB console. */
void activity_set_debug(bool enabled);

#endif /* TRINKET_ACTIVITY_H */
