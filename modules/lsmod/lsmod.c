#include "../../common/myrtos_abi.h"

// lsmod -- lists the module directory. OS-9 had the same tool under the name
// mdir, and for the same reason: the modules are the system's real table of
// contents.
//
// Each line is built complete before it is sent. A write is atomic, but a line
// made of several writes can be broken up by other processes.
void module_main(void) {
    int32_t t = myrtos_console();
    if (t < 0) { myrtos_exit(); return; }

    myrtos_line_t line;
    myrtos_line_reset(&line);
    myrtos_line_str(&line, "\nModule directory:\n  name          links\n");
    myrtos_line_flush(t, &line);

    char name[12];
    for (uint32_t i = 0; ; i++) {
        int32_t links = myrtos_moddir_get(i, name);
        if (links < 0) break;
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  ");
        myrtos_line_chars(&line, name, 11);
        myrtos_line_str(&line, "   ");
        myrtos_line_u32(&line, (uint32_t)links);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(t, &line);
    }
    // The path is not closed: it was inherited and belongs to whoever started us.
}
