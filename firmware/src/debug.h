/*
 * debug.h -- globally gated USB-serial diagnostics.
 */

#ifndef TRINKET_DEBUG_H
#define TRINKET_DEBUG_H

#include <stdbool.h>

void debug_set_enabled(bool enabled);
bool debug_is_enabled(void);
void debug_printf(const char *format, ...);

#endif /* TRINKET_DEBUG_H */
