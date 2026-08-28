#include "../../common/myrtos_abi.h"

// mfree -- största sammanhängande fria block, och hur många processer som
// lever. Det största blocket är det tal som betyder något i ett realtidssystem:
// fragmentering syns där, inte i summan.
void module_main(void) {
    int32_t t = myrtos_console();
    if (t < 0) { myrtos_exit(); return; }

    myrtos_line_t line;
    myrtos_line_reset(&line);
    myrtos_line_str(&line, "\nLargest free block: ");
    myrtos_line_u32(&line, (uint32_t)myrtos_meminfo(MYRTOS_MEM_LARGEST_FREE));
    myrtos_line_str(&line, " bytes\n");
    myrtos_line_flush(t, &line);

    myrtos_line_reset(&line);
    myrtos_line_str(&line, "Processes alive:    ");
    myrtos_line_u32(&line, (uint32_t)myrtos_meminfo(MYRTOS_MEM_PROCESSES));
    myrtos_line_str(&line, "\n");
    myrtos_line_flush(t, &line);

    // Vägen stängs inte: den ärvdes och tillhör den som startade oss.
}
