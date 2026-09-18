// The wall clock. See clock.h for what it is for.
//
// The time is kept as one number -- the UTC second that was true at a known
// uptime -- and read by adding however long the machine has run since. That
// makes every reading derive from the monotonic timer, so the clock cannot go
// backwards between answers from the network, and a missed answer costs
// accuracy rather than correctness.

#include "clock.h"
#include "pico/time.h"

static uint32_t base_utc;      // the second we were told
static uint64_t base_us;       // the uptime at which it was true
static bool     have_it;
static int32_t  offset_min;    // from config.txt, east of UTC

bool ubiqos_clock_is_set(void) { return have_it; }
int32_t ubiqos_clock_offset(void) { return offset_min; }
void ubiqos_clock_set_offset(int32_t minutes) { offset_min = minutes; }

void ubiqos_clock_set(uint32_t utc_seconds)
{
    base_utc = utc_seconds;
    base_us  = time_us_64();
    have_it  = true;
}

uint32_t ubiqos_clock_utc(void)
{
    if (!have_it) return 0;
    return base_utc + (uint32_t)((time_us_64() - base_us) / 1000000u);
}

uint32_t ubiqos_clock_local(void)
{
    if (!have_it) return 0;
    return (uint32_t)((int32_t)ubiqos_clock_utc() + offset_min * 60);
}

// Days since the epoch to a civil date. Howard Hinnant's algorithm, which
// shifts the year to start in March so that the leap day is the last day of it
// and every month before it has a fixed length. No tables and no loop over
// years, which matters because this is called from the logging path.
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d)
{
    z += 719468;
    const int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    const uint32_t doe = (uint32_t)(z - era * 146097);
    const uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int32_t yr = (int32_t)yoe + era * 400;
    const uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const uint32_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : -9);
    *y = yr + (*m <= 2);
}

static char *two(char *p, uint32_t v)
{
    *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    return p;
}

void ubiqos_clock_stamp(char *out, uint32_t cap)
{
    if (cap < 20) { if (cap) out[0] = 0; return; }
    if (!have_it) {
        const char *unset = "--------- --:--:--";
        uint32_t i = 0;
        while (unset[i]) { out[i] = unset[i]; i++; }
        out[i] = 0;
        return;
    }

    const uint32_t t = ubiqos_clock_local();
    const uint32_t secs_of_day = t % 86400u;
    int32_t y; uint32_t mo, d;
    civil_from_days((int32_t)(t / 86400u), &y, &mo, &d);

    char *p = out;
    *p++ = (char)('0' + (uint32_t)(y / 1000) % 10);
    *p++ = (char)('0' + (uint32_t)(y / 100) % 10);
    p = two(p, (uint32_t)y);
    *p++ = '-'; p = two(p, mo);
    *p++ = '-'; p = two(p, d);
    *p++ = ' ';
    p = two(p, secs_of_day / 3600u);
    *p++ = ':'; p = two(p, (secs_of_day / 60u) % 60u);
    *p++ = ':'; p = two(p, secs_of_day % 60u);
    *p = 0;
}

void ubiqos_clock_time_only(char *out, uint32_t cap)
{
    if (cap < 9) { if (cap) out[0] = 0; return; }
    if (!have_it) { out[0] = 0; return; }

    const uint32_t s = ubiqos_clock_local() % 86400u;
    char *p = out;
    p = two(p, s / 3600u);
    *p++ = ':'; p = two(p, (s / 60u) % 60u);
    *p++ = ':'; p = two(p, s % 60u);
    *p = 0;
}
