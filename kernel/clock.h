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

// Where the machine is: its offset from UTC and, if it has one, its summer
// time and the rules for when that starts and ends. From config.txt's
// timezone; see ubiqos_clock_parse_zone for what it accepts.
typedef struct {
    uint8_t month;     // 1..12
    uint8_t week;      // 1..5, 5 being the last in the month
    uint8_t wday;      // 0..6, Sunday first
    int32_t secs;      // local time of day the change happens, in seconds
} ubiqos_zone_rule_t;

typedef struct {
    int32_t std_min;   // standard time, minutes east of UTC
    int32_t dst_min;   // summer time, minutes east of UTC
    bool    has_dst;
    ubiqos_zone_rule_t start, end;   // start in standard time, end in summer time
    char    std_name[8], dst_name[8];
} ubiqos_zone_t;

// "+2", "-3:30" or "0": a fixed offset, hours east of UTC, with no summer
// time. Or a POSIX TZ rule, as Linux, ESP-IDF and Zephyr take it:
// "CET-1CEST,M3.5.0,M10.5.0/3" is Sweden. Its offset counts hours WEST, as
// POSIX's always has, which is why CET is -1. True if it is one of the two.
bool ubiqos_clock_parse_zone(const char *text, uint32_t len, ubiqos_zone_t *zone);
void ubiqos_clock_set_zone(const ubiqos_zone_t *zone);

// The offset from UTC in minutes that is true now, summer time counted.
// Positive is east.
int32_t ubiqos_clock_offset(void);

// The name of the time that is true now -- "CET" or "CEST" -- or "" for a
// zone given as a bare offset.
const char *ubiqos_clock_zone_name(void);

// "2026-09-11 15:42:07" into at least 20 bytes, or "--:--:--" when unset.
void ubiqos_clock_stamp(char *out, uint32_t cap);

// Just "15:42:07" into at least 9 bytes. Empty when unset, so a caller can
// print it unconditionally and get nothing before the network answers.
void ubiqos_clock_time_only(char *out, uint32_t cap);

#endif
