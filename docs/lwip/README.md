# lwIP as a position-independent module

Measured against lwIP 2.x as it ships with the Pico SDK, 23 files of `src/core`,
`src/core/ipv4` and `src/netif/ethernet.c`, compiled with the module flags
(`-fno-pic -mcmodel=medany -fno-common -ffreestanding -nostdlib -O2`).

    before   4707 relocations, 15 disallowed
    after    4726 relocations,  0 disallowed

Fifteen, out of four and a half thousand. lwIP is very nearly position
independent as it stands, and the reason is worth knowing: it keeps its callbacks
in **variables** -- `pcb->recv` is assigned at run time -- and a function pointer
written at run time costs nothing in relocations. Only *statically initialised*
tables produce absolute addresses. lwIP also dispatches with if-chains and direct
calls rather than tables. It was written for embedded targets where being
ROM-able mattered, which is the same discipline for a different reason.

All fifteen were two tables in `core/tcp.c`, and they needed different answers.

## `tcp_state_str[]` -- eleven strings

`UBIQOS_RELTAB_*` from `common/ubiqos_abi.h` does exactly this. The table stores
the DISTANCE from itself to each string, and the strings live in the module image
beside it, so that distance is a link-time constant. Eleven `R_RISCV_32` become
eleven ADD32/SUB32 pairs, which carry no absolute address.

The strings had to be given names first: a string literal is anonymous (`.LC0`)
and the assembler cannot take its difference.

## `tcp_pcb_lists[]` -- four pointers to globals

A relative table does **not** work here, and the reason is the interesting part.
Those globals are writable, so in a module they live in each process's own data
area. The distance from a shared `.rodata` table to them is not the same for
every process, and a fixed number cannot express it. `UBIQOS_RELTAB_*` is for
code and read-only data, which move together.

A function replaces the table, returning the address the compiler would have
computed anyway. Written as a `switch` first, GCC gathered the three addresses
into a lookup table in `.rodata` -- the very thing being avoided -- so it is an
if-chain with `optimize("O0")` on it. It is called rarely and the cost is
nothing.

## What this does not show

Relocations only. lwIP's writable globals are a separate question: as a module
they would each become per-process, which is right for one service process and
wrong for a library everyone links -- another reason the stack wants to be a
process of its own. Seven of thirty files also did not compile, purely for want
of a real `cc.h`; that is porting work, not a matter of principle.

## Building it as a module: where it stands

The port is written and works: `lwipopts.h`, `arch/cc.h`, `sys_arch.c` for
`sys_now` and the randomness mDNS wants, and a small string and libc for a module
linked `-nostdlib` -- lwIP needs `memcpy` and friends, `atoi`, `snprintf`, and
newlib's `_ctype_` table. All 35 files compile, and the module links.

Zeroconf is configured for from the start, since it dictates options: mDNS
insists on `LWIP_IGMP` for IPv4, on `LWIP_UDP`, on a source of randomness for the
delay before it answers, and on a per-netif client data slot.

**57 absolute addresses remain**, in five tables, and they are the same kind of
thing tcp.c already carried: `memp_pools[]` and the pool descriptors (44),
`lwip_cyclic_timers[]` with its handler pointers (7), a few strings in mDNS (5),
and one of my own -- newlib's `_ctype_` is a pointer, and a pointer initialised
with an address is a relocation.

**But that is not what stops it.** A flash-resident module executes in place at
0x10100000, so its `.data` would be in flash and writes to it would go nowhere.
The check's refusal of writable sections is a hardware fact rather than a matter
of taste, and the SINGLE attribute was written without asking why the rule was
there. It is still right that a service may be single-instance; it is wrong that
being single-instance makes shared writable data work.

What would actually make this run, in rough order of size:

  - a loader that copies a SINGLE module into RAM instead of running it in
    place, which is what makes writable data writable at all; or
  - lwIP's globals in thread-local storage, which is where a module's writable
    state already goes, and which needs no loader change -- but there is no
    compiler flag for "put this translation unit's globals in .tdata".

The five tables are the smaller half of the work and the technique for them is
known. The loader question is the real one.
