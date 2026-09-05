//! zhello -- the first myrtos module written in Zig.
//!
//! Like dhello it exists to prove a toolchain, so it uses only the things that
//! could go wrong: the trap, the command line, a variable at module scope, and
//! the kernel's random source.
//!
//! The variable is the point. `calls` below is `threadlocal`, and Zig gives that
//! TPREL relocations on a freestanding riscv32 target with no flag asked for --
//! the same ones __thread produces in C, and the ones LDC only produces when
//! told --fthread-model=local-exec.
//!
//! Take the keyword away and it becomes an ordinary shared static. That is
//! allowed: the module is marked private and copied per process, so the count
//! is still per process -- but a copy of the whole module is a poor way to buy
//! four bytes. Zig has no way to make every variable thread-local; that remains
//! D's alone.
const m = @import("myrtos");

comptime {
    asm (
        \\.globl __myrtos_mem_size
        \\.set __myrtos_mem_size, 4096
    );
}

// No annotation would make this shared between every process running zhello.
// Two shells running it at once each count their own.
threadlocal var calls: u32 = 0;

export fn module_main(argc: i32, argv: [*][*:0]const u8) void {
    calls += 1;

    m.writeStr(m.STDOUT, "zhello: from Zig, call ");
    var num: [12]u8 = undefined;
    m.writeStr(m.STDOUT, m.u32ToDec(calls, &num));
    m.writeStr(m.STDOUT, " in this process\n");

    var i: usize = 1;
    while (i < @as(usize, @intCast(argc))) : (i += 1) {
        m.writeStr(m.STDOUT, "zhello:   arg ");
        m.writeStr(m.STDOUT, m.u32ToDec(@intCast(i), &num));
        m.writeStr(m.STDOUT, " = ");
        m.writeStr(m.STDOUT, cstr(argv[i]));
        m.writeStr(m.STDOUT, "\n");
    }

    var bytes: [8]u8 = undefined;
    if (m.random(&bytes) > 0) {
        m.writeStr(m.STDOUT, "zhello: eight from the ring oscillator:");
        for (bytes) |b| {
            m.writeStr(m.STDOUT, " ");
            m.writeStr(m.STDOUT, m.u32ToDec(b, &num));
        }
        m.writeStr(m.STDOUT, "\n");
    }
}

fn cstr(p: [*:0]const u8) []const u8 {
    var n: usize = 0;
    while (p[n] != 0) n += 1;
    return p[0..n];
}
