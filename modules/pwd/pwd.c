#include "../../common/myrtos_abi.h"

// pwd -- prints the current directory.
//
// Unlike cd this can be a module: it only reads the directory it inherited at
// exec, and reading a copy tells you as much as reading the original. It is cd
// that has to be built into the shell, because changing a copy changes nothing.
void module_main(void) {
    char cwd[64];
    myrtos_getcwd(cwd, sizeof(cwd));
    myrtos_write_str(MYRTOS_STDOUT, cwd[0] ? cwd : "/");
    myrtos_write_str(MYRTOS_STDOUT, "\n");
}
