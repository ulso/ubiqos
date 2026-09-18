#include "crashlog.h"
#include "pico/platform/sections.h"

// A crash used to leave nothing behind. The machine sat in the trap handler's
// wfi loop with a black screen and no serial port, and the only way in was the
// probe -- which could read memory perfectly well, but had nowhere to look. An
// hour went into a register dump that said only that we had panicked.
//
// So the crash writes itself down first, in a named global the probe can find
// by symbol. The board still parks exactly as before; nothing about the failure
// changes. What changes is that afterwards there is something to read.
//
// The most useful field is the panic's own format string. The SDK's panics are
// all literals -- "hardware alarm %d already claimed", the clock one, the
// malloc one -- so the pointer alone names the caller, without needing to
// decode a return address or trust a stack that may be the reason we are here.
// In memory the startup code does NOT clear, so it survives a reboot.
//
// It used to be an ordinary global, readable only by a probe attached to a
// board still sitting in the fault -- which is exactly the board somebody has
// just pressed BOOTSEL on to get it back. Twice in one day the evidence was
// destroyed by the act of recovering the machine, and both times the answer to
// "why did it stop" was thrown away with it.
//
// Uninitialised RAM keeps its contents across a warm reset, so the crash is
// still there at the next boot and ubiqos_crash_report puts it in the log. A
// power cycle still loses it, and that is the honest limit.
__uninitialized_ram(ubiqos_crash_t) ubiqos_crash;

uint32_t ubiqos_asserts_seen;
uint32_t ubiqos_assert_last;

void panic(const char *fmt, ...);   // the real one, reached through --wrap

void __real_panic(const char *fmt, ...);

// The first writer wins, and that is the whole point. A panic ends in ebreak,
// which is a trap, which lands in the handler -- so the second thing to happen
// would otherwise erase the first, and the first is the one that says why. What
// the probe read before this rule was "breakpoint at _exit": true, useless, and
// exactly as far as an hour with the register dump had already got.
void ubiqos_crash_note (uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (ubiqos_crash.magic == UBIQOS_CRASH_MAGIC) return;
    ubiqos_crash.kind = kind;
    ubiqos_crash.a = a;
    ubiqos_crash.b = b;
    ubiqos_crash.c = c;
    ubiqos_crash.magic = UBIQOS_CRASH_MAGIC;   // last, so a half-written one is not believed
}

void __wrap_panic (const char *fmt, ...)
{
    ubiqos_crash_note (UBIQOS_CRASH_PANIC, (uint32_t)fmt, (uint32_t)__builtin_return_address (0), 0);
    __real_panic ("%s", fmt ? fmt : "(no message)");
}


// Said once, at the next boot, and then forgotten. A crash that is reported
// every time from then on is a crash nobody reads after the second boot.
void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);

void ubiqos_crash_report(void)
{
    if (ubiqos_crash.magic != UBIQOS_CRASH_MAGIC) return;

    ubiqos_print("crash: the last run ended in ");
    switch (ubiqos_crash.kind) {
    case UBIQOS_CRASH_PANIC:  ubiqos_print("a panic"); break;
    case UBIQOS_CRASH_TRAP:   ubiqos_print("a trap"); break;
    case UBIQOS_CRASH_ASSERT: ubiqos_print("an assert"); break;
    default:                  ubiqos_print("something unnamed"); break;
    }

    // The three words raw, because what they mean depends on the kind and a
    // wrong label is worse than a number. crashlog.h says which is which.
    ubiqos_print(" -- ");
    ubiqos_print_u32(ubiqos_crash.a);
    ubiqos_print(" ");
    ubiqos_print_u32(ubiqos_crash.b);
    ubiqos_print(" ");
    ubiqos_print_u32(ubiqos_crash.c);
    ubiqos_print("\n");

    ubiqos_crash.magic = 0;
}
