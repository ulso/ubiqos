#include "../../common/ubiqos_abi.h"

// faultdemo -- runs for three seconds and then executes an undefined
// instruction, on purpose. It is here to be ended by the kernel and restarted
// by init, which is the only way to see that both do what they say.
UBIQOS_MEM_SIZE(1024);

void module_main(int argc, char **argv) {
    (void)argc; (void)argv;
    ubiqos_write_str(UBIQOS_STDOUT, "faultdemo: running; faulting in three seconds\n");
    ubiqos_sleep(3000);
    __builtin_trap();
}
