#include "../../common/ubiqos_abi.h"

// path -- where a program that is not in flash is looked for.
//
//     path                  show it
//     path /sd/bin:/sd      set it
//     path ""               flash only
//
// A module rather than a shell builtin, because the path is not the shell's:
// it is one list for the whole machine, kept by the loader in the kernel. That
// is what lets /sd/startup set it and have the setting outlive the startup
// file -- a per-process path, as Unix has, would have ended with it.
void module_main(int argc, char **argv) {
    if (ubiqos_help(argc, argv,
            "usage: path [dir:dir:...]\n\n"
            "Shows or sets where a program that is not in flash is looked for.\n"
            "Each directory names its volume first, and they are tried in order:\n"
            "  path /sd/bin:/sd\n"
            "Programs in flash are always found first. The default is /sd, the\n"
            "root of the card; an empty path (path \"\") means flash only.\n"))
        return;

    if (argc > 2) {
        ubiqos_write_str(UBIQOS_STDERR, "path: one argument, with colons between the directories\n");
        return;
    }
    if (argc == 2) {
        // path "" arrives as one empty argument: the quotes go at exec.
        if (ubiqos_path_set(argv[1]) != 0) {
            ubiqos_write_str(UBIQOS_STDERR,
                "path: each directory must start with its volume, as in /sd/bin,\n"
                "      and the whole path must be shorter than 128 characters\n");
            return;
        }
    }

    char now[UBIQOS_PATH_MAX];
    if (ubiqos_path_get(now, sizeof now) != 0) {
        ubiqos_write_str(UBIQOS_STDERR, "path: the loader did not answer\n");
        return;
    }
    ubiqos_write_str(UBIQOS_STDOUT, now[0] ? now : "(flash only)");
    ubiqos_write_str(UBIQOS_STDOUT, "\n");
}
