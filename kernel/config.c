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
#include "config.h"
#include "fat32.h"
#include "clock.h"

void ubiqos_print(const char *s);

#define CONFIG_PATH "config.txt"

static char host[32] = "ubiqos";
static char ssid[33];
static char pass[64];
static bool done;

// 192.168.7.1, the address a Linux board in USB gadget mode takes, so that the
// computer gets 192.168.7.2. A subnet of its own for the cable -- see
// kernel/lwipdhcpd.c for why 169.254 was not one.
#define USB_ADDRESS_DEFAULT ((192u << 24) | (168u << 16) | (7u << 8) | 1u)
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
static void take_timezone(const char *v, uint32_t n) {
    uint32_t i = 0;
    int32_t sign = 1;
    if (i < n && (v[i] == '+' || v[i] == '-')) { if (v[i] == '-') sign = -1; i++; }

    int32_t hours = 0, mins = 0;
    uint32_t digits = 0;
    while (i < n && v[i] >= '0' && v[i] <= '9') { hours = hours * 10 + (v[i++] - '0'); digits++; }
    if (!digits) { ubiqos_print("config: that timezone is not a number; staying on UTC\n"); return; }

    if (i < n && v[i] == ':') {
        i++;
        digits = 0;
        while (i < n && v[i] >= '0' && v[i] <= '9') { mins = mins * 10 + (v[i++] - '0'); digits++; }
        if (!digits) mins = 0;
    }

    if (i != n || hours > 14 || mins > 59) {
        ubiqos_print("config: that timezone is not an offset; staying on UTC\n");
        return;
    }
    ubiqos_clock_set_offset(sign * (hours * 60 + mins));
}

// "usb_address = 10.0.5.1": four numbers, each 0 to 255, and nothing else. Not
// every address will do. Loopback, multicast and the reserved blocks are not
// addresses a link can have; 169.254 is what this replaced; and the last number
// must leave room for the computer's, which is one more, below the broadcast
// address of the /24.
static void take_usb_address(const char *v, uint32_t n) {
    uint32_t a = 0, i = 0;
    for (int part = 0; part < 4; part++) {
        uint32_t x = 0, digits = 0;
        while (i < n && v[i] >= '0' && v[i] <= '9' && digits < 4) { x = x * 10 + (uint32_t)(v[i++] - '0'); digits++; }
        if (!digits || x > 255) goto bad;
        a = (a << 8) | x;
        if (part < 3) { if (i >= n || v[i] != '.') goto bad; i++; }
    }
    if (i != n) goto bad;
    {
        const uint32_t first = a >> 24, last = a & 0xffu;
        if (first == 0 || first == 127 || first >= 224) goto bad;
        if ((a >> 16) == ((169u << 8) | 254u)) goto bad;
        if (last == 0 || last > 253) goto bad;
    }
    usb_address = a;
    return;
bad:
    ubiqos_print("config: that usb_address is not one to use; keeping 192.168.7.1\n");
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
        else ubiqos_print("config: that hostname is not a name; keeping ubiqos\n");
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
static void read_the_file(void)
{
    const ubiqos_fsops_t *ops = ubiqos_fat_ops_ptr();
    if (!ops || !ops->read_at) return;

    uint32_t size = 0;
    if (ubiqos_fat_stat(CONFIG_PATH, &size) < 0) return;   // no file, nothing to say

    // Read in pieces and assemble lines, rather than the whole file at once:
    // this runs on the filesystem server's four kilobytes of stack, and a
    // buffer big enough for any config.txt anybody might write is exactly the
    // kind of thing that fits until the day it does not.
    static char line[160];
    uint8_t chunk[64];
    uint32_t fill = 0, at = 0;
    bool overlong = false;

    while (at < size) {
        int32_t got = ops->read_at(CONFIG_PATH, at, chunk, sizeof(chunk));
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

    ubiqos_print("config: hostname ");
    ubiqos_print(host);
    if (!ssid[0])       ubiqos_print(", no network named\n");
    else if (!pass[0])  ubiqos_print(", a network but no password\n");
    else                ubiqos_print(", network and password\n");
}

void ubiqos_config_read(void)
{
    read_the_file();

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
