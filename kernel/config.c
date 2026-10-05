// /sd/config.txt, read once at boot.
//
//     # what this machine is called and what network it is on
//     hostname = jamboree
//     ssid     = the-network
//     password = ...
//     timezone = +2
//     usb_address = 192.168.7.1
//
// Keys are case-insensitive, everything after '#' is a comment, and a value
// runs to the end of the line with the spaces either side trimmed. Trailing
// spaces in a password are trimmed too, and that is a deliberate trade: a
// password ending in a space cannot be written here, and in exchange a file
// saved by an editor that pads its lines still works.
//
// --- ON KEEPING THE PASSWORD -----------------------------------------------
//
// Ulf's rule for `wifi connect` is that the password is typed at the board's own
// keyboard: never echoed, never an argument, never over the serial port. Putting
// it in a file weakens that unless the file is treated differently from every
// other file, so it is:
//
//   * the kernel reads it HERE, through the FAT library directly, and never
//     hands the bytes to a process;
//   * the filesystem server refuses to open or read /sd/config.txt for anybody
//     else -- see is_secret in fsserver.c -- so `cat`, `more` and `cp` cannot
//     see it. It can still be WRITTEN, so an editor can replace it;
//   * `usbdisk` is the hole that remains, and it is left open on purpose. It
//     hands the whole card to a host as a block device, which is what it is
//     for; anybody who can type that command can read the card in a reader
//     anyway.
//
// The password never leaves this file's statics except into ubiqos_wifi_join.

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "fat32.h"
#include "clock.h"
#include "tlsf.h"
#include "flashmod.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/platform.h"
#include "hardware/regs/addressmap.h"
#include "../common/ubiqos_abi.h"

void ubiqos_print(const char *s);

#define CONFIG_PATH "config.txt"

// The network's name and password, kept apart from everything else.
//
// They were in config.txt and are still read from there, because cards exist
// that were written before this file did and a board that stops joining after
// a software update is a bad trade for tidiness. This one is read afterwards
// and wins. What it buys is that the file holding the secret can be handled as
// one thing -- taken out, replaced, and one day written sealed -- while the
// settings that are nobody's secret stay in a file anybody may read.
#define WIFI_PATH "wificfg.txt"

// The board's own name when the card gives none, where the board has one -- a
// board with no card slot at all would otherwise answer to "ubiqos", like
// every other board in the room with nothing on its card.
#include "board.h"
#ifndef UBIQOS_DEFAULT_HOSTNAME
#define UBIQOS_DEFAULT_HOSTNAME "ubiqos"
#endif
static char host[32] = UBIQOS_DEFAULT_HOSTNAME;
static char ssid[33];
static char pass[64];
static bool done;

// 192.168.7.1, the address a Linux board in USB gadget mode takes, so that the
// computer gets 192.168.7.2. A subnet of its own for the cable -- see
// kernel/lwipdhcpd.c for why 169.254 was not one.
//
// A board header may name another, for the same reason it may name the host:
// two boards on one computer that both say 192.168.7.1 give it two cables to
// one subnet, and it answers one board down the other's cable. The card's
// usb_address still wins where there is a card.
#ifdef UBIQOS_DEFAULT_USB_ADDRESS
#define USB_ADDRESS_DEFAULT UBIQOS_DEFAULT_USB_ADDRESS
#else
#define USB_ADDRESS_DEFAULT ((192u << 24) | (168u << 16) | (7u << 8) | 1u)
#endif
static uint32_t usb_address = USB_ADDRESS_DEFAULT;
uint32_t ubiqos_config_usb_address(void) { return usb_address; }

bool ubiqos_config_done(void) { return done; }
void ubiqos_config_give_up(void) { done = true; }
const char *ubiqos_config_hostname(void) { return host; }
const char *ubiqos_config_ssid(void)     { return ssid; }
bool ubiqos_config_has_password(void)    { return pass[0] != 0; }

const char *ubiqos_config_credentials(void)
{
    if (!ssid[0] || !pass[0]) return 0;

    static char creds[sizeof(ssid) + sizeof(pass)];
    uint32_t n = 0;
    for (uint32_t i = 0; ssid[i]; i++) creds[n++] = ssid[i];
    creds[n++] = 0;
    for (uint32_t i = 0; pass[i]; i++) creds[n++] = pass[i];
    creds[n] = 0;
    return creds;
}

static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

// The key, compared without regard to case and with the spaces already gone.
static bool key_is(const char *k, uint32_t n, const char *want) {
    for (uint32_t i = 0; i < n; i++) {
        if (!want[i] || lower(k[i]) != want[i]) return false;
    }
    return want[n] == 0;
}

static void copy_into(char *dst, uint32_t cap, const char *src, uint32_t n) {
    if (n > cap - 1) n = cap - 1;
    for (uint32_t i = 0; i < n; i++) dst[i] = src[i];
    dst[n] = 0;
}

// A hostname is also an mDNS label and a DNS name, so it may hold letters,
// digits and hyphens and nothing else. Something else in the file is a mistake
// worth saying out loud rather than a name worth announcing: a label with a
// space in it would be refused by the responder anyway, and silently falling
// back would leave the card looking as if it had not been read.
static bool valid_hostname(const char *s, uint32_t n) {
    if (n == 0 || n > sizeof(host) - 1) return false;
    for (uint32_t i = 0; i < n; i++) {
        char c = lower(s[i]);
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return s[0] != '-' && s[n - 1] != '-';
}

// "timezone = +2" or "-3:30" or "0". Hours east of UTC, with optional minutes.
//
// No daylight saving: the board would have to carry the rules for wherever it
// is and the date they change, and it has neither. So this is the offset that
// is true today, and it is the user's to edit twice a year. Said plainly here
// because a clock that is quietly an hour out is worse than one that is
// obviously unset.
static bool parse_timezone(const char *v, uint32_t n, int32_t *minutes) {
    uint32_t i = 0;
    int32_t sign = 1;
    if (i < n && (v[i] == '+' || v[i] == '-')) { if (v[i] == '-') sign = -1; i++; }

    int32_t hours = 0, mins = 0;
    uint32_t digits = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9' && digits < 3) { hours = hours * 10 + (v[i++] - '0'); digits++; }
    if (!digits) return false;

    if (i < n && v[i] == ':') {
        i++;
        digits = 0;
        while (i < n && v[i] >= '0' && v[i] <= '9' && digits < 3) { mins = mins * 10 + (v[i++] - '0'); digits++; }
        if (!digits) mins = 0;
    }

    if (i != n || hours > 14 || mins > 59) return false;
    *minutes = sign * (hours * 60 + mins);
    return true;
}

static void take_timezone(const char *v, uint32_t n) {
    int32_t minutes;
    if (parse_timezone(v, n, &minutes)) ubiqos_clock_set_offset(minutes);
    else ubiqos_print("config: that timezone is not an offset; leaving the clock as it was\n");
}

// "usb_address = 10.0.5.1": four numbers, each 0 to 255, and nothing else. Not
// every address will do. Loopback, multicast and the reserved blocks are not
// addresses a link can have; 169.254 is what this replaced; and the last number
// must leave room for the computer's, which is one more, below the broadcast
// address of the /24.
static bool parse_usb_address(const char *v, uint32_t n, uint32_t *out) {
    uint32_t a = 0, i = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t x = 0, digits = 0;
        while (i < n && v[i] >= '0' && v[i] <= '9' && digits < 4) { x = x * 10 + (uint32_t)(v[i++] - '0'); digits++; }
        if (!digits || x > 255) return false;
        a = (a << 8) | x;
        if (part < 3) { if (i >= n || v[i] != '.') return false; i++; }
    }
    if (i != n) return false;
    const uint32_t first = a >> 24, last = a & 0xffu;
    if (first == 0 || first == 127 || first >= 224) return false;
    if ((a >> 16) == ((169u << 8) | 254u)) return false;
    if (last == 0 || last > 253) return false;
    *out = a;
    return true;
}

void ubiqos_config_address_text(uint32_t a, char out[16]) {
    uint32_t n = 0;
    for (int part = 3; part >= 0; part--) {
        const uint32_t x = (a >> (part * 8)) & 0xffu;
        if (x >= 100) out[n++] = (char)('0' + x / 100);
        if (x >= 10)  out[n++] = (char)('0' + (x / 10) % 10);
        out[n++] = (char)('0' + x % 10);
        if (part) out[n++] = '.';
    }
    out[n] = 0;
}

// What is kept is said, and not a number that was true on another board: this
// said "keeping 192.168.7.1" on the Challenger, whose own default is .8.1.
static void take_usb_address(const char *v, uint32_t n) {
    uint32_t a;
    if (parse_usb_address(v, n, &a)) { usb_address = a; return; }
    char t[16];
    ubiqos_config_address_text(usb_address, t);
    ubiqos_print("config: that usb_address is not one to use; keeping ");
    ubiqos_print(t);
    ubiqos_print("\n");
}

// One line, already stripped of its newline.
static void take_line(char *l, uint32_t n) {
    // A comment starts at a '#' that begins the line or follows a blank, and
    // NOT at one in the middle of a word.
    //
    // It used to be any '#' at all, which is wrong in a file that carries a
    // password: a perfectly good secret with a '#' in it was silently cut
    // short there, and the only symptom was a network that would not join.
    // Nothing said the password had been shortened, because nothing knew.
    //
    // What is left of the old behaviour is the one case a config file wants:
    // "ssid = home # the one downstairs" still ends at the hash. So a password
    // may hold '#' anywhere except directly after a space, and that is the
    // whole of the remaining trade -- stated here because the alternative is
    // finding it out at two in the morning.
    for (uint32_t i = 0; i < n; i++) {
        if (l[i] != '#') continue;
        if (i == 0 || l[i - 1] == ' ' || l[i - 1] == '\t') { n = i; break; }
    }
    uint32_t k = 0;
    while (k < n && (l[k] == ' ' || l[k] == '\t')) k++;

    uint32_t key = k;
    while (k < n && l[k] != '=' && l[k] != ' ' && l[k] != '\t') k++;
    uint32_t keylen = k - key;
    if (!keylen) return;                                // blank, or all comment

    while (k < n && (l[k] == ' ' || l[k] == '\t')) k++;
    if (k >= n || l[k] != '=') return;                  // not a setting
    k++;
    while (k < n && (l[k] == ' ' || l[k] == '\t')) k++;

    uint32_t val = k;
    while (n > val && (l[n - 1] == ' ' || l[n - 1] == '\t' || l[n - 1] == '\r')) n--;
    uint32_t vallen = n - val;
    if (!vallen) return;                                // "ssid =" says nothing

    if (key_is(l + key, keylen, "hostname")) {
        if (valid_hostname(l + val, vallen)) copy_into(host, sizeof(host), l + val, vallen);
        else { ubiqos_print("config: that hostname is not a name; keeping "); ubiqos_print(host); ubiqos_print("\n"); }
    } else if (key_is(l + key, keylen, "ssid")) {
        copy_into(ssid, sizeof(ssid), l + val, vallen);
    } else if (key_is(l + key, keylen, "password")) {
        copy_into(pass, sizeof(pass), l + val, vallen);
    } else if (key_is(l + key, keylen, "timezone")) {
        take_timezone(l + val, vallen);
    } else if (key_is(l + key, keylen, "usb_address")) {
        take_usb_address(l + val, vallen);
    }
    // Anything else is somebody else's setting, or a typo. Neither is worth
    // refusing the rest of the file over.
}

// The reading itself. ubiqos_config_read wraps it and is the only thing that
// says the card has been looked at -- see the note there.
static bool read_the_file(const char *path)
{
    const ubiqos_fsops_t *ops = ubiqos_fat_ops_ptr();
    if (!ops || !ops->read_at) return false;

    uint32_t size = 0;
    if (ubiqos_fat_stat(path, &size) < 0) return false;    // no file, nothing to say

    // Read in pieces and assemble lines, rather than the whole file at once:
    // this runs on the filesystem server's four kilobytes of stack, and a
    // buffer big enough for any config.txt anybody might write is exactly the
    // kind of thing that fits until the day it does not.
    static char line[160];
    uint8_t chunk[64];
    uint32_t fill = 0, at = 0;
    bool overlong = false;

    while (at < size) {
        int32_t got = ops->read_at(path, at, chunk, sizeof(chunk));
        if (got <= 0) break;
        at += (uint32_t)got;
        for (int32_t i = 0; i < got; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                if (!overlong) take_line(line, fill);
                fill = 0; overlong = false;
                continue;
            }
            if (fill < sizeof(line)) line[fill++] = c;
            else overlong = true;       // and the whole line is discarded
        }
    }
    if (fill && !overlong) take_line(line, fill);        // no newline at the end
    return true;
}

// --- SETTINGS KEPT IN FLASH ------------------------------------------------
//
// A board with no card has nowhere to keep config.txt, so it keeps the part
// of it that is nobody's secret in a flash sector of its own: hostname,
// usb_address and timezone, as the same text the file would hold. Read here
// before the card, so that a card, where there is one, still has the last word.
// Written by `config set` through the filesystem server, since an erase is tens
// of milliseconds and no trap's business -- see ubiqos_config_store.
//
// The sector is a header and the text: a magic number, the length, and an
// FNV-1a sum over the text, so that a sector half written when the power went
// reads as no settings rather than as some.
#define CFG_MAGIC     0x47464355u          // "UCFG"
#define CFG_TEXT_MAX  1024u

typedef struct { uint32_t magic, len, sum; } cfg_head_t;

// What is written and what is borrowed from the pool to write it: the header
// and the most text there may be, rounded up to the unit the flash programs
// in. Not the sector, which on the STM32F4 is 16 kB -- more than the Feather
// STM32F405 had free with sshd running. The rest of the sector stays erased.
#define CFG_IMAGE_BYTES \
    ((sizeof(cfg_head_t) + CFG_TEXT_MAX + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE)
_Static_assert(CFG_IMAGE_BYTES <= FLASH_SECTOR_SIZE, "the settings must fit their sector");

static uint32_t fnv1a(const char *p, uint32_t n) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < n; i++) { h ^= (uint8_t)p[i]; h *= 16777619u; }
    return h;
}

// The text kept in flash, or 0 when there is none or it does not add up.
static const char *stored_text(uint32_t *len) {
    const cfg_head_t *h = (const cfg_head_t *)(uintptr_t)UBIQOS_FLASH_CONFIG_BASE;
    if (h->magic != CFG_MAGIC || h->len > CFG_TEXT_MAX) return 0;
    const char *t = (const char *)(h + 1);
    if (fnv1a(t, h->len) != h->sum) return 0;
    *len = h->len;
    return t;
}

int32_t ubiqos_config_stored(char *out, uint32_t cap) {
    uint32_t n = 0;
    const char *t = stored_text(&n);
    if (!t || !cap) return 0;
    if (n > cap - 1) n = cap - 1;
    memcpy(out, t, n);
    out[n] = 0;
    return (int32_t)n;
}

// The key a line sets, or 0 when it sets none. What take_line reads, without
// the acting on it.
static const char *line_key(const char *l, uint32_t n, uint32_t *keylen) {
    uint32_t k = 0;
    while (k < n && (l[k] == ' ' || l[k] == '\t')) k++;
    const uint32_t key = k;
    while (k < n && l[k] != '=' && l[k] != ' ' && l[k] != '\t') k++;
    *keylen = k - key;
    return *keylen ? l + key : 0;
}

// Every line of a text, as the file's lines are taken.
static uint32_t take_text(const char *t, uint32_t n) {
    static char line[160];
    uint32_t fill = 0, lines = 0;
    for (uint32_t i = 0; i <= n; i++) {
        if (i == n || t[i] == '\n') {
            if (fill) { take_line(line, fill); lines++; }
            fill = 0;
            continue;
        }
        if (fill < sizeof line) line[fill++] = t[i];
    }
    return lines;
}

// Same copy as the key store's: erase, then program, with this core's
// interrupts off throughout, from code that is not in the flash being erased.
static void __not_in_flash_func(write_sector)(uint32_t offset, const uint8_t *data) {
    const uint32_t st = save_and_disable_interrupts();
    flash_range_erase(offset, FLASH_SECTOR_SIZE);
    if (data) flash_range_program(offset, data, CFG_IMAGE_BYTES);
    restore_interrupts(st);
}

extern tlsf_pool_t ubiqos_mem_pool;

// Keep KEY = VALUE, replacing what was kept for it, or with an empty VALUE
// forget it. Checked as config.txt's own reading checks it, so that what is
// kept is always something the next start will take. Nothing changes until
// that start: the name has been announced, and the address is the link's.
int32_t ubiqos_config_store(const char *key, const char *value) {
    // usb_address only where there is a network on the USB cable; see
    // UBIQOS_HAS_USB_NETWORK.
#if !UBIQOS_HAS_USB_NETWORK
    static const char *const kept[] = { "hostname", "timezone" };
#else
    static const char *const kept[] = { "hostname", "usb_address", "timezone" };
#endif
    const uint32_t klen = (uint32_t)strlen(key), vlen = (uint32_t)strlen(value);
    const char *canon = 0;
    for (uint32_t i = 0; i < sizeof kept / sizeof kept[0]; i++)
        if (key_is(key, klen, kept[i])) canon = kept[i];
    if (!canon) return UBIQOS_CFG_ENOKEY;

    if (vlen) {
        uint32_t a;
        int32_t m;
        const bool ok = canon == kept[0]                ? valid_hostname(value, vlen)
                      : strcmp(canon, "usb_address") == 0 ? parse_usb_address(value, vlen, &a)
                      :                                     parse_timezone(value, vlen, &m);
        if (!ok) return UBIQOS_CFG_EVALUE;
    }

    uint8_t *img = ubiqos_tlsf_malloc(ubiqos_mem_pool, CFG_IMAGE_BYTES);
    if (!img) return UBIQOS_CFG_ENOMEM;
    memset(img, 0xff, CFG_IMAGE_BYTES);
    char *text = (char *)(img + sizeof(cfg_head_t));
    uint32_t n = 0;
    bool full = false;

    // What was kept, less this key's line.
    uint32_t oldn = 0;
    const char *old = stored_text(&oldn);
    for (uint32_t at = 0; old && at < oldn; ) {
        uint32_t end = at;
        while (end < oldn && old[end] != '\n') end++;
        uint32_t kl;
        const char *k = line_key(old + at, end - at, &kl);
        if (k && !key_is(k, kl, canon)) {
            if (n + (end - at) + 1 > CFG_TEXT_MAX) full = true;
            else { memcpy(text + n, old + at, end - at); n += end - at; text[n++] = '\n'; }
        }
        at = end + 1;
    }
    // And this one, last.
    if (vlen) {
        const uint32_t cl = (uint32_t)strlen(canon);
        if (n + cl + 3 + vlen + 1 > CFG_TEXT_MAX) full = true;
        else {
            memcpy(text + n, canon, cl); n += cl;
            memcpy(text + n, " = ", 3); n += 3;
            memcpy(text + n, value, vlen); n += vlen;
            text[n++] = '\n';
        }
    }
    if (full) { ubiqos_tlsf_free(ubiqos_mem_pool, img); return UBIQOS_CFG_EFULL; }

    cfg_head_t *h = (cfg_head_t *)img;
    h->magic = CFG_MAGIC;
    h->len = n;
    h->sum = fnv1a(text, n);

    // Nothing left to keep is an erased sector, which reads as none.
    const uint32_t offset = UBIQOS_FLASH_CONFIG_BASE - XIP_BASE;
    write_sector(offset, n ? img : 0);

    const uint8_t *now = (const uint8_t *)(uintptr_t)UBIQOS_FLASH_CONFIG_BASE;
    const bool same = n ? memcmp(now, img, CFG_IMAGE_BYTES) == 0
                        : ((const cfg_head_t *)now)->magic == 0xffffffffu;
    ubiqos_tlsf_free(ubiqos_mem_pool, img);
    return same ? 0 : UBIQOS_CFG_EFLASH;
}

void ubiqos_config_read(void)
{
    // Flash first; the card after it wins.
    uint32_t kept_len = 0;
    const char *kept = stored_text(&kept_len);
    if (kept) {
        const uint32_t lines = take_text(kept, kept_len);
        ubiqos_print(lines == 1 ? "config: 1 setting kept in flash\n" : "config: settings kept in flash\n");
    }

    read_the_file(CONFIG_PATH);

    // And then the network's own file, which wins: a card that has both is one
    // where the credentials have been moved out and the old lines forgotten,
    // and the file that was written on purpose is the one to believe.
    const bool named_by_config = ssid[0] != 0;
    const bool has_wifi_file   = read_the_file(WIFI_PATH);

    ubiqos_print("config: hostname ");
    ubiqos_print(host);
    if (!ssid[0])       ubiqos_print(", no network named\n");
    else if (!pass[0])  ubiqos_print(", a network but no password\n");
    else if (named_by_config && !has_wifi_file)
                        ubiqos_print(", network and password (from config.txt)\n");
    else                ubiqos_print(", network and password\n");

    // LAST, and this is the whole point of the wrapper.
    //
    // It used to be the first line of the reading, so that an early return
    // still counted as having tried -- and that is exactly wrong. Reading the
    // card takes milliseconds and this thread blocks for them, so the USB task
    // ran in between, saw "done", and started lwIP with the hostname nobody
    // had read yet. mDNS announces once and cannot unsay a name: the log
    // showed "answering to ubiqos.local" three lines ABOVE "config: hostname
    // jamboree", and jamboree.local did not exist.
    //
    // A race that usually wins is worse than one that never does. It was right
    // the first time it was tested, which is why it survived.
    done = true;
}
