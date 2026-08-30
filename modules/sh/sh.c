#include "../../common/myrtos_abi.h"

// The myrtos shell. OS-9's shell did essentially this: read a name, look the
// module up, start it. All the complexity is in the kernel; the shell is a loop.

// The prompt says where we are, because with directories there is now somewhere
// to be. The root shows as a bare "/" rather than nothing at all.
static void prompt(int32_t c) {
    char cwd[64];
    myrtos_getcwd(cwd, sizeof(cwd));
    myrtos_write_str(c, "\r\nmyrtos:");
    myrtos_write_str(c, cwd[0] ? cwd : "/");
    myrtos_write_str(c, "> ");
}

// cd is built in and has to be. A module changing its own current directory
// changes nothing for the shell that started it -- the child gets a copy at
// exec and takes it to the grave.
static void change_dir(int32_t c, const char *line) {
    const char *arg = line;
    while (*arg && *arg != ' ') arg++;
    while (*arg == ' ') arg++;
    if (!*arg) arg = "/";

    if (myrtos_chdir(arg) == 0) return;

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "cd: no such directory: ");
    myrtos_line_str(&l, arg);
    myrtos_line_str(&l, "\r\n");
    myrtos_line_flush(c, &l);
}

static void help(int32_t c) {
    myrtos_write_str(c,
        // The name column is nine wide because bootsel is seven characters and
        // needs two after it. Everything lines up on the same column.
        "\r\nType a module name to run it. Built in:\r\n"
        "  help     this text\r\n"
        "  lsmod    list modules\r\n"
        "  ps       list processes\r\n"
        "  keys     read the USB keyboard\r\n"
        "  free     memory and processes\r\n"
        "  echo     print its arguments\r\n"
        "  ls       list a directory\r\n"
        "  cd       change directory (built in)\r\n"
        "  pwd      where you are\r\n"
        "  mkdir    make a directory\r\n"
        "  rmdir    remove an empty directory\r\n"
        "  mount    take the SD card again\r\n"
        "  cat      show a file\r\n"
        "  cp       copy a file\r\n"
        "  rm       delete a file\r\n"
        "  write    write text to a file\r\n"
        "  sleep    wait, in milliseconds\r\n"
        "  nice     run a command at a priority\r\n"
        "  bootsel  reboot into the bootloader\r\n"
        "\r\nA trailing & runs a command without waiting for it.\r\n");
}

// Split the line at the first space: everything before is the module name,
// everything after is the command line the process is given.
static int32_t exec_line(char *line) {
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) { *args = 0; args++; while (*args == ' ') args++; }

    // A trailing & means do not wait. Without it there is no way to have two
    // processes running at once from the keyboard, and no way to see that the
    // sleep list orders more than one sleeper.
    bool background = false;
    char *end = args;
    while (*end) end++;
    while (end > args && end[-1] == ' ') end--;
    if (end > args && end[-1] == '&') {
        background = true;
        end[-1] = 0;
        while (end > args && end[-1] == ' ') { end--; *end = 0; }
    }

    int32_t pid = myrtos_exec(line, args);
    // Wait for it before prompting again. Without this the prompt raced the
    // command's own output, and two commands in a row interleaved their lines.
    if (pid >= 0 && !background) myrtos_wait(pid);
    return pid;
}

static bool line_is(const char *line, const char *word) {
    int i = 0;
    while (line[i] && word[i] && line[i] == word[i]) i++;
    return !line[i] && !word[i];
}

static bool line_starts(const char *line, const char *word) {
    while (*word) { if (*line != *word) return false; line++; word++; }
    return true;
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
                } else if (line_is(line, "cd") || line_starts(line, "cd ")) {
                    change_dir(c, line);
                } else if (exec_line(line) < 0) {
                    myrtos_line_t l;
                    myrtos_line_reset(&l);
                    myrtos_line_str(&l, "no such module: ");
                    myrtos_line_str(&l, line);   // exec_line NUL-terminated the name
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
            myrtos_write(c, &ch, 1);   // echo it
        }
    }
}
