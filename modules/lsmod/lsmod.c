#include "../../common/myrtos_abi.h"

// lsmod -- lists the module directory. OS-9 had the same tool under the name
// mdir, and for the same reason: the modules are the system's real table of
// contents.
//
// The revision earns its column. A name exists once, and which copy the system
// kept is decided by that number -- so a module that was replaced by a patched
// one is invisible here except through it.
//
// Each line is built complete before it is sent. A write is atomic, but a line
// made of several writes can be broken up by other processes.

static void pad(myrtos_line_t *l, uint32_t written, uint32_t width) {
    while (written++ < width) myrtos_line_str(l, " ");
}

static uint32_t digits(uint32_t v) {
    uint32_t n = 1;
    while (v >= 10) { v /= 10; n++; }
    return n;
}

void module_main(void) {
    myrtos_line_t line;
    myrtos_line_reset(&line);
    myrtos_line_str(&line, "\nModule directory:\n"
                           "  name      type  rev  links  bytes\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);

    for (uint32_t i = 0; ; i++) {
        myrtos_modinfo_t m;
        if (myrtos_moddir_get(i, &m) < 0) break;

        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  ");
        // Eight characters, not eleven: the last three are the 8.3 extension,
        // and it says MOD on every module ever made. What the reader wants
        // there is the type, which the header knows and the name never did.
        myrtos_line_chars(&line, m.name, 8);
        myrtos_line_str(&line, "  ");
        myrtos_line_str(&line, myrtos_type_name(m.type));
        myrtos_line_str(&line, "   ");
        myrtos_line_u32(&line, m.revision);
        pad(&line, digits(m.revision), 5);
        myrtos_line_u32(&line, m.links);
        pad(&line, digits(m.links), 7);
        myrtos_line_u32(&line, m.size);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(MYRTOS_STDOUT, &line);
    }
}
