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
                           "  name             type  rev  links  bytes\n");
    myrtos_line_flush(MYRTOS_STDOUT, &line);

    for (uint32_t i = 0; ; i++) {
        myrtos_modinfo_t m;
        if (myrtos_moddir_get(i, &m) < 0) break;

        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  ");
        // The name as it is, then spaces to the column. It used to be eight
        // characters copied raw, which worked only because a name was padded to
        // eleven in store -- the last three being the 8.3 extension, which said
        // MOD on every module ever made. Names are terminated now, so copying a
        // fixed count would put the terminator and whatever follows it on the
        // screen.
        uint32_t namelen = 0;
        while (m.name[namelen]) namelen++;
        myrtos_line_chars(&line, m.name, namelen);
        pad(&line, namelen, 17);
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
