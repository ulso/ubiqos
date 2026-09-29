// The sizes of the kernel's own tables, per board.
//
// Each has a default that suits a board with memory to spare, and a board
// header may name a smaller one as UBIQOS_BOARD_<NAME>. That is all the
// mechanism there is: a line in boards/<board>.h, read by the compiler. What a
// board leaves out it gets the default for.
//
// Every kernel file that sizes a table or walks one takes the number from
// here, and this takes the board header itself. Two files that disagreed about
// a table's size would compile without a word and write past its end when run
// -- so nothing may define these anywhere else, and the board header is never
// read around this file.
//
// A board with 32 kB of SRAM is the reason: with the defaults, these tables
// and the stacks are 47 kB on their own.
#ifndef UBIQOS_SIZES_H
#define UBIQOS_SIZES_H

#include "board.h"
#include "../common/ubiqos_abi.h"

// Processes, the kernel's own threads included: the idle kernel, the file
// server and the console take three before anything is typed. A slot costs
// its bookkeeping here and a row of paths in io.c. UBIQOS_MAX_PROCESSES in the ABI
// stays the ceiling -- it is how far a module walks when it lists processes,
// and a slot this kernel does not have answers -1 like an empty one.
#ifdef UBIQOS_BOARD_PROCESSES
#define UBIQOS_PROCESSES UBIQOS_BOARD_PROCESSES
#else
#define UBIQOS_PROCESSES UBIQOS_MAX_PROCESSES
#endif
_Static_assert(UBIQOS_PROCESSES <= UBIQOS_MAX_PROCESSES,
               "more processes than a module walks: raise UBIQOS_MAX_PROCESSES");
_Static_assert(UBIQOS_PROCESSES >= 5,
               "the kernel, file server and console take three; a shell and a command need two more");

// Paths a process may have open: stdin, stdout and stderr take three.
#ifdef UBIQOS_BOARD_PATHS
#define UBIQOS_PATHS UBIQOS_BOARD_PATHS
#else
#define UBIQOS_PATHS 12
#endif
_Static_assert(UBIQOS_PATHS >= 4, "stdin, stdout and stderr take three");

// Files open at once, across every process. 72 bytes each.
#ifdef UBIQOS_BOARD_OPEN_FILES
#define UBIQOS_OPEN_FILES UBIQOS_BOARD_OPEN_FILES
#else
#define UBIQOS_OPEN_FILES 16
#endif

// The kernel log's ring, /var/dmesg. The build may already have chosen one --
// the RP2350's follows the video mode -- and a board that names its own wins.
#ifdef UBIQOS_BOARD_DMESG_SIZE
#undef DMESG_SIZE
#define DMESG_SIZE UBIQOS_BOARD_DMESG_SIZE
#elif !defined(DMESG_SIZE)
#define DMESG_SIZE 2048
#endif

// The stack every exception and system call runs on, on Arm. See
// kernel/arm/stack.c for what has to fit.
#ifdef UBIQOS_BOARD_IRQ_STACK_BYTES
#define UBIQOS_IRQ_STACK_BYTES UBIQOS_BOARD_IRQ_STACK_BYTES
#else
#define UBIQOS_IRQ_STACK_BYTES 4096
#endif
_Static_assert(UBIQOS_IRQ_STACK_BYTES % 8 == 0, "the stack is 8-byte aligned");

#endif
