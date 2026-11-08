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
static ubiqos_zone_t zone;     // from config.txt; all zero is UTC

bool ubiqos_clock_is_set(void) { return have_it; }
void ubiqos_clock_set_zone(const ubiqos_zone_t *z) { zone = *z; }

static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d);
static void civil_from_days(int32_t z, int32_t *y, uint32_t *m, uint32_t *d);

// --- summer time -------------------------------------------------------------
//
// A rule is POSIX's Mm.w.d: day d of week w of month m, week 5 meaning the
// last such day. The board carries no table of the world's zones -- the rule
// is in the configuration, and the board only works out this year's two dates
// from it, which is all a table would have given it.

static uint32_t month_days(int32_t y, uint32_t m)
{
    static const uint8_t len[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    const bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return len[m - 1] + (m == 2 && leap);
}

// The UTC second at which the rule's change happens in year y, the local time
// of day it names being in a clock that is `before_min` east of UTC.
static int64_t change_at(int32_t y, const ubiqos_zone_rule_t *r, int32_t before_min)
{
    const int32_t first = days_from_civil(y, r->month, 1);
    const uint32_t first_wday = (uint32_t)(((first % 7) + 11) % 7);   // 1970-01-01 was a Thursday
    uint32_t day = 1 + (r->wday + 7 - first_wday) % 7 + (uint32_t)(r->week - 1) * 7;
    while (day > month_days(y, r->month)) day -= 7;
    return ((int64_t)first + day - 1) * 86400 + r->secs - (int64_t)before_min * 60;
}

static bool in_summer(uint32_t utc)
{
    if (!zone.has_dst) return false;
    int32_t y; uint32_t m, d;
    civil_from_days((int32_t)(((int64_t)utc + zone.std_min * 60) / 86400), &y, &m, &d);
    const int64_t start = change_at(y, &zone.start, zone.std_min);
    const int64_t end = change_at(y, &zone.end, zone.dst_min);
    const int64_t t = utc;
    // North of the equator summer is inside the year; south, it spans New Year.
    return start < end ? (t >= start && t < end) : (t >= start || t < end);
}

int32_t ubiqos_clock_offset(void)
{
    return have_it && in_summer(ubiqos_clock_utc()) ? zone.dst_min : zone.std_min;
}

const char *ubiqos_clock_zone_name(void)
{
    return have_it && in_summer(ubiqos_clock_utc()) ? zone.dst_name : zone.std_name;
}

// --- reading the zone --------------------------------------------------------
//
// Run once at boot and by `config set`, never from an interrupt, so it stays
// in flash rather than being copied into SRAM with the rest of the kernel:
// see kernel/ld/section_copy_to_ram_rodata.incl. What runs on every reading
// of the clock -- in_summer and what it calls -- is above, and is copied.
#if defined(__ELF__)
#define IN_FLASH __attribute__((section(".text.flash.clock")))
#else
#define IN_FLASH
#endif

typedef struct { const char *s; uint32_t n, i; } cursor_t;

IN_FLASH static bool more(const cursor_t *c) { return c->i < c->n; }
IN_FLASH static char peek(const cursor_t *c) { return more(c) ? c->s[c->i] : 0; }
IN_FLASH static bool digit(char ch) { return ch >= '0' && ch <= '9'; }

IN_FLASH static bool number(cursor_t *c, uint32_t max_digits, int32_t *v)
{
    uint32_t got = 0;
    int32_t x = 0;
    while (got < max_digits && digit(peek(c))) { x = x * 10 + (c->s[c->i++] - '0'); got++; }
    *v = x;
    return got > 0;
}

// [+-]hh[:mm[:ss]] as seconds. Hours may run to 167, as a rule's time may.
IN_FLASH static bool hms(cursor_t *c, int32_t *secs)
{
    int32_t sign = 1, h, m = 0, s = 0;
    if (peek(c) == '+' || peek(c) == '-') { if (peek(c) == '-') sign = -1; c->i++; }
    if (!number(c, 3, &h) || h > 167) return false;
    if (peek(c) == ':') { c->i++; if (!number(c, 2, &m) || m > 59) return false; }
    if (peek(c) == ':') { c->i++; if (!number(c, 2, &s) || s > 59) return false; }
    *secs = sign * (h * 3600 + m * 60 + s);
    return true;
}

// A zone's name: three or more letters, or anything in <> -- "<+0530>".
IN_FLASH static bool zone_name(cursor_t *c, char *out, uint32_t cap)
{
    uint32_t k = 0;
    if (peek(c) == '<') {
        c->i++;
        while (more(c) && peek(c) != '>') {
            const char ch = c->s[c->i++];
            const bool ok = digit(ch) || ch == '+' || ch == '-' ||
                            (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            if (!ok || k + 1 >= cap) return false;
            out[k++] = ch;
        }
        if (peek(c) != '>') return false;
        c->i++;
    } else {
        while ((peek(c) >= 'a' && peek(c) <= 'z') || (peek(c) >= 'A' && peek(c) <= 'Z')) {
            if (k + 1 >= cap) return false;
            out[k++] = c->s[c->i++];
        }
    }
    out[k] = 0;
    return k >= 3;
}

// ",Mm.w.d[/time]". Only that form: the Julian-day ones are for places that
// change on a fixed date, and none of the zones anybody here lives in does.
IN_FLASH static bool zone_rule(cursor_t *c, ubiqos_zone_rule_t *r)
{
    int32_t m, w, d;
    if (peek(c) != ',') return false;
    c->i++;
    if (peek(c) != 'M') return false;
    c->i++;
    if (!number(c, 2, &m) || m < 1 || m > 12 || peek(c) != '.') return false;
    c->i++;
    if (!number(c, 1, &w) || w < 1 || w > 5 || peek(c) != '.') return false;
    c->i++;
    if (!number(c, 1, &d) || d > 6) return false;
    r->month = (uint8_t)m; r->week = (uint8_t)w; r->wday = (uint8_t)d;
    r->secs = 2 * 3600;                        // POSIX's default: two in the morning
    if (peek(c) == '/') { c->i++; if (!hms(c, &r->secs)) return false; }
    return true;
}

IN_FLASH bool ubiqos_clock_parse_zone(const char *text, uint32_t len, ubiqos_zone_t *out)
{
    ubiqos_zone_t z = { 0 };
    cursor_t c = { text, len, 0 };
    if (!len) return false;

    // A bare offset, east of UTC, as config.txt has always taken it.
    if (digit(text[0]) || text[0] == '+' || text[0] == '-') {
        int32_t sign = 1, h, m = 0;
        if (peek(&c) == '+' || peek(&c) == '-') { if (peek(&c) == '-') sign = -1; c.i++; }
        if (!number(&c, 3, &h)) return false;
        if (peek(&c) == ':') { c.i++; if (!number(&c, 3, &m)) m = 0; }
        if (more(&c) || h > 14 || m > 59) return false;
        z.std_min = z.dst_min = sign * (h * 60 + m);
        *out = z;
        return true;
    }

    // A POSIX rule. Its offsets count west, so they are turned round here.
    int32_t secs;
    if (!zone_name(&c, z.std_name, sizeof z.std_name) || !hms(&c, &secs)) return false;
    z.std_min = z.dst_min = -secs / 60;
    if (!more(&c)) { *out = z; return true; }

    if (!zone_name(&c, z.dst_name, sizeof z.dst_name)) return false;
    z.dst_min = z.std_min + 60;                // POSIX's default: an hour ahead
    if (peek(&c) != ',') {
        if (!hms(&c, &secs)) return false;
        z.dst_min = -secs / 60;
    }
    // A summer time with no rule would be POSIX's "implementation-defined",
    // which here would mean guessing. Better to be told.
    if (!zone_rule(&c, &z.start) || !zone_rule(&c, &z.end) || more(&c)) return false;
    if (z.std_min < -14 * 60 || z.std_min > 14 * 60 || z.dst_min < -14 * 60 || z.dst_min > 14 * 60)
        return false;
    z.has_dst = true;
    *out = z;
    return true;
}

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
    const uint32_t utc = ubiqos_clock_utc();
    const int32_t off = in_summer(utc) ? zone.dst_min : zone.std_min;
    return (uint32_t)((int64_t)utc + off * 60);
}

// A civil date to days since the epoch: Hinnant's again, the other way.
static int32_t days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
    y -= m <= 2;
    const int32_t era = (y >= 0 ? y : y - 399) / 400;
    const uint32_t yoe = (uint32_t)(y - era * 400);
    const uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const uint32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int32_t)doe - 719468;
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
