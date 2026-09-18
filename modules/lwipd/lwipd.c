#include "../../common/ubiqos_abi.h"
#include "lwip/init.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"

// lwipd -- the TCP/IP stack, as a process.
//
// Declared SINGLE, and it has to be: lwIP keeps its pcbs, its ARP table and its
// memory pools in globals, and a second instance sharing them would be two
// stacks writing one set of tables. One process owns the stack, and clients will
// reach it by message rather than by linking against it.
//
// There is no network interface yet -- the ESP32-C6 hangs off SPI and its driver
// is not written -- so this brings the stack up, services its timers, and proves
// it runs. That is the milestone: lwIP as a position-independent UbiqOS module.
void module_main(void) {
    ubiqos_line_t line;

    lwip_init();

    ubiqos_line_reset(&line);
    ubiqos_line_str(&line, "lwipd: stack up, pid ");
    ubiqos_line_u32(&line, (uint32_t)ubiqos_pidof("lwipd"));

    ubiqos_line_str(&line, ", no interface yet\n");
    ubiqos_line_flush(UBIQOS_STDOUT, &line);

    // lwIP with NO_SYS has no threads of its own: its timers only run when it is
    // asked. Ten milliseconds is far finer than anything it schedules -- ARP is
    // measured in seconds -- and costs nothing at this priority.
    for (;;) {
        sys_check_timeouts();
        ubiqos_sleep(10);
    }
}
