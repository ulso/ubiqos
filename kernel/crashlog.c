#include "crashlog.h"

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
myrtos_crash_t myrtos_crash;

void panic(const char *fmt, ...);   // the real one, reached through --wrap

void __real_panic(const char *fmt, ...);

// The first writer wins, and that is the whole point. A panic ends in ebreak,
// which is a trap, which lands in the handler -- so the second thing to happen
// would otherwise erase the first, and the first is the one that says why. What
// the probe read before this rule was "breakpoint at _exit": true, useless, and
// exactly as far as an hour with the register dump had already got.
void myrtos_crash_note (uint32_t kind, uint32_t a, uint32_t b, uint32_t c)
{
    if (myrtos_crash.magic == MYRTOS_CRASH_MAGIC) return;
    myrtos_crash.kind = kind;
    myrtos_crash.a = a;
    myrtos_crash.b = b;
    myrtos_crash.c = c;
    myrtos_crash.magic = MYRTOS_CRASH_MAGIC;   // last, so a half-written one is not believed
}

void __wrap_panic (const char *fmt, ...)
{
    myrtos_crash_note (MYRTOS_CRASH_PANIC, (uint32_t)fmt, (uint32_t)__builtin_return_address (0), 0);
    __real_panic ("%s", fmt ? fmt : "(no message)");
}
