#include "../../common/myrtos_abi.h"

// The myrtos shell. OS-9's shell did essentially this: read a name, look the
// module up, start it. All the complexity is in the kernel; the shell is a loop.

static void prompt(int32_t c) {
    myrtos_write_str(c, "\r\nmyrtos> ");
}

static void help(int32_t c) {
    myrtos_write_str(c,
        "\r\nType a module name to run it. Built in:\r\n"
        "  help   this text\r\n"
        "  lsmod  list modules\r\n"
        "  free   memory and processes\r\n"
        "  echo   print its arguments\r\n"
        "  ls     list the SD card\r\n"
        "  cat    show a file\r\n");
}

// Split the line at the first space: everything before is the module name,
// everything after is the command line the process is given.
static int32_t exec_line(char *line) {
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) { *args = 0; args++; while (*args == ' ') args++; }
    return myrtos_exec(line, args);
}

static bool line_is(const char *line, const char *word) {
    int i = 0;
    while (line[i] && word[i] && line[i] == word[i]) i++;
    return !line[i] && !word[i];
}

void module_main(void) {
    // The kernel has already given us 0, 1 and 2. The shell opens nothing.
    const int32_t c = MYRTOS_STDOUT;

    myrtos_write_str(c, "\r\nmyrtos shell ready. Type 'help'.\r\n");
    prompt(c);

    char line[48];
    uint32_t len = 0;

    for (;;) {
        uint8_t ch;
        if (myrtos_read(MYRTOS_STDIN, &ch, 1) <= 0) continue;   // nothing right now

        if (ch == '\r' || ch == '\n') {
            line[len] = 0;
            if (len) {
                myrtos_write_str(c, "\r\n");
                if (line_is(line, "help")) {
                    help(c);
                } else if (exec_line(line) < 0) {
                    myrtos_line_t l;
                    myrtos_line_reset(&l);
                    myrtos_line_str(&l, "no such module: ");
                    myrtos_line_str(&l, line);   // exec_line nollterminerade namnet
                    myrtos_line_str(&l, "\r\n");
                    myrtos_line_flush(c, &l);
                }
            }
            len = 0;
            prompt(c);
        } else if (ch == 8 || ch == 127) {
            if (len) { len--; myrtos_write_str(c, "\b \b"); }
        } else if (ch >= ' ' && len < sizeof(line) - 1) {
            line[len++] = (char)ch;
            myrtos_write(c, &ch, 1);   // eka tecknet
        }
    }
}
