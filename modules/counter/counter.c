#include <stdint.h>
#include "../../common/myrtos_abi.h"

// Systemanropen och deras nummer kommer från det gemensamma ABI:t. Modulen
// definierar dem inte längre själv, så en ändring i kärnan ger byggfel i
// stället för ett obegripligt fel vid körning.

void module_main(void) {
    int32_t path = myrtos_open("term");
    if (path < 0) return;

    myrtos_write_str(path, "\n[counter] second module, loaded from the same card\n");

    for (int i = 0; i < 6; i++) {
        char line[] = "[counter] tick N of 6\n";
        line[15] = (char)('1' + i);   // 'N' sitter på 15, inte 18
        myrtos_write_str(path, line);
        // Bränn tid så att kvantumet löper ut mitt i räkningen.
        for (volatile int d = 0; d < 300000; d++) { }
    }

    myrtos_write_str(path, "[counter] done\n");
    myrtos_close(path);
}
