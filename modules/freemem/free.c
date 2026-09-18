#include "../../common/ubiqos_abi.h"

// free -- the largest contiguous free block, in both pools, and how many
// processes are alive. The largest block is the number that matters in a
// real-time system: fragmentation shows there, not in the total.

static void row(const char *label, uint32_t v, const char *unit) {
    ubiqos_line_t l;
    ubiqos_line_reset(&l);
    ubiqos_line_str(&l, label);
    ubiqos_line_u32(&l, v);
    ubiqos_line_str(&l, unit);
    ubiqos_line_str(&l, "\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &l);
}

void module_main(void) {
    row("SRAM  largest free: ", (uint32_t)ubiqos_meminfo(UBIQOS_MEM_LARGEST_FREE), " bytes");

    uint32_t bulk = (uint32_t)ubiqos_meminfo(UBIQOS_MEM_BULK_SIZE);
    if (bulk) {
        row("PSRAM largest free: ", (uint32_t)ubiqos_meminfo(UBIQOS_MEM_BULK_FREE), " bytes");
        row("PSRAM total:        ", bulk / 1024, " kB");
    } else {
        ubiqos_write_str(UBIQOS_STDOUT, "PSRAM: none\n");
    }

    row("Processes alive:    ", (uint32_t)ubiqos_meminfo(UBIQOS_MEM_PROCESSES), "");

    // Only when there have been any. A line reading zero every time is a line
    // nobody reads, and this one has to be noticed on the day it is not zero.
    uint32_t asserts = (uint32_t)ubiqos_meminfo(UBIQOS_MEM_ASSERTS);
    if (asserts) {
        row("Assertions stepped: ", asserts, "");
        ubiqos_line_t l;
        ubiqos_line_reset(&l);
        ubiqos_line_str(&l, "  last at ");
        ubiqos_line_hex(&l, (uint32_t)ubiqos_meminfo(UBIQOS_MEM_ASSERT_LAST));
        ubiqos_line_str(&l, "\n");
        ubiqos_line_flush(UBIQOS_STDOUT, &l);
    }
    // The path is not closed: it was inherited and belongs to whoever started us.
}
