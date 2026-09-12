// The board this kernel is being built for.
//
// CMake chooses it -- see MYRTOS_BOARD -- and passes the answer as
// MYRTOS_BOARD_HEADER. Nothing in the kernel names a board directly; it asks
// this header for a number or a fact and gets whichever machine is being built.
//
// The contract a board header owes is in boards/fruit-jam.h, which is also the
// commentary on why each entry is there. In short: a number that changes when
// the board changes belongs there, and a number a DRIVER owns does not -- those
// arrive in the driver's descriptor, which is a data module and so already
// per-board.
#ifndef MYRTOS_BOARD_H
#define MYRTOS_BOARD_H

#ifndef MYRTOS_BOARD_HEADER
#error "MYRTOS_BOARD_HEADER is not set; CMake should have defined it"
#endif

#include MYRTOS_BOARD_HEADER

#endif
