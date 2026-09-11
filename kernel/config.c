// /sd/config.txt, read once at boot.
//
//     # what this machine is called and what network it is on
//     hostname = jamboree
//     ssid     = the-network
//     password = ...
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
// The password never leaves this file's statics except into myrtos_wifi_join.

#include <stdint.h>
#include <stdbool.h>
#include "config.h"
#include "fat32.h"

void myrtos_print(const char *s);

#define CONFIG_PATH "config.txt"

static char host[32] = "myrtos";
static char ssid[33];
static char pass[64];
static bool done;

bool myrtos_config_done(void) { return done; }
void myrtos_config_give_up(void) { done = true; }
const char *myrtos_config_hostname(void) { return host; }
const char *myrtos_config_ssid(void)     { return ssid; }
bool myrtos_config_has_password(void)    { return pass[0] != 0; }

const char *myrtos_config_credentials(void)
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
        else myrtos_print("config: that hostname is not a name; keeping myrtos\n");
    } else if (key_is(l + key, keylen, "ssid")) {
        copy_into(ssid, sizeof(ssid), l + val, vallen);
    } else if (key_is(l + key, keylen, "password")) {
        copy_into(pass, sizeof(pass), l + val, vallen);
    }
    // Anything else is somebody else's setting, or a typo. Neither is worth
    // refusing the rest of the file over.
}

// The reading itself. myrtos_config_read wraps it and is the only thing that
// says the card has been looked at -- see the note there.
static void read_the_file(void)
{
    const myrtos_fsops_t *ops = myrtos_fat_ops_ptr();
    if (!ops || !ops->read_at) return;

    uint32_t size = 0;
    if (myrtos_fat_stat(CONFIG_PATH, &size) < 0) return;   // no file, nothing to say

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

    myrtos_print("config: hostname ");
    myrtos_print(host);
    if (!ssid[0])       myrtos_print(", no network named\n");
    else if (!pass[0])  myrtos_print(", a network but no password\n");
    else                myrtos_print(", network and password\n");
}

void myrtos_config_read(void)
{
    read_the_file();

    // LAST, and this is the whole point of the wrapper.
    //
    // It used to be the first line of the reading, so that an early return
    // still counted as having tried -- and that is exactly wrong. Reading the
    // card takes milliseconds and this thread blocks for them, so the USB task
    // ran in between, saw "done", and started lwIP with the hostname nobody
    // had read yet. mDNS announces once and cannot unsay a name: the log
    // showed "answering to myrtos.local" three lines ABOVE "config: hostname
    // jamboree", and jamboree.local did not exist.
    //
    // A race that usually wins is worse than one that never does. It was right
    // the first time it was tested, which is why it survived.
    done = true;
}
