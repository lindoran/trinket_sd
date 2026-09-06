/*
 * debug.c -- globally gated USB-serial diagnostics.
 */

#include "debug.h"

#include <stdarg.h>
#include <stdio.h>

static bool debug_enabled;

void debug_set_enabled(bool enabled) {
    debug_enabled = enabled;
}

bool debug_is_enabled(void) {
    return debug_enabled;
}

void debug_printf(const char *format, ...) {
    if (!debug_enabled) {
        return;
    }

    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}
