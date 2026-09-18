// A wall clock, separate from the monotonic tick.
//
// The machine has no battery-backed clock, so it boots knowing only how long it
// has been running. Something on the network has to tell it the date -- see
// kernel/lwipsntp.c -- and until that happens every function here says so
// rather than inventing a plausible year.
#ifndef UBIQOS_CLOCK_H
#define UBIQOS_CLOCK_H

#include <stdint.h>
#include <stdbool.h>

// True once somebody has told us the time.
bool ubiqos_clock_is_set(void);

// Seconds since 1970-01-01 UTC, or 0 when unset.
uint32_t ubiqos_clock_utc(void);

// The same with the configured offset applied. This is what a person reads.
uint32_t ubiqos_clock_local(void);

// Set from UTC seconds. Records the uptime it was true at, so the clock keeps
// running between one answer from the network and the next.
void ubiqos_clock_set(uint32_t utc_seconds);

// The offset from UTC in minutes, from config.txt. Positive is east.
void ubiqos_clock_set_offset(int32_t minutes);
int32_t ubiqos_clock_offset(void);

// "2026-09-11 15:42:07" into at least 20 bytes, or "--:--:--" when unset.
void ubiqos_clock_stamp(char *out, uint32_t cap);

// Just "15:42:07" into at least 9 bytes. Empty when unset, so a caller can
// print it unconditionally and get nothing before the network answers.
void ubiqos_clock_time_only(char *out, uint32_t cap);

#endif
