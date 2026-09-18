#include <stdint.h>
#include "../../common/ubiqos_abi.h"

// The system calls and their numbers come from the shared ABI. The module no
// longer defines them itself, so a change in the kernel gives a build error
// rather than an incomprehensible failure at runtime.

void module_main(void) {
    int32_t path = ubiqos_open("/dev/term");
    if (path < 0) return;

    ubiqos_write_str(path, "\n[counter] second module, loaded from the same card\n");

    for (int i = 0; i < 6; i++) {
        char line[] = "[counter] tick N of 6\n";
        line[15] = (char)('1' + i);   // 'N' sits at 15, not 18
        ubiqos_write_str(path, line);
        // Burn time so the quantum expires in the middle of the count.
        for (volatile int d = 0; d < 300000; d++) { }
    }

    ubiqos_write_str(path, "[counter] done\n");
    ubiqos_close(path);
}
