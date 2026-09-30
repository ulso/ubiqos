#include "../../common/ubiqos_abi.h"

// The table init starts a node from: one program a line, in the order they
// are started. See modules/init for what each column means.
//
// This one is the node example on the NUCLEO-H503RB: nodelog first, because
// the others talk to it, and faultdemo, which faults on purpose, so that what
// init does when a program dies can be seen.
__attribute__((section(".rodata.descriptor"), used))
const char inittab[] =
    "# program   priority  restart  after     arguments\n"
    "nodelog     20        always   -\n"
    "nodebeat    16        always   nodelog\n"
    "faultdemo   16        fault    nodelog\n";
