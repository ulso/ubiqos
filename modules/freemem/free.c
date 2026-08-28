#include "../../common/myrtos_abi.h"

// free -- the largest contiguous free block, and how many processes are alive.
// The largest block is the number that matters in a real-time system:
// fragmentation shows there, not in the total.
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

    // The path is not closed: it was inherited and belongs to whoever started us.
}
