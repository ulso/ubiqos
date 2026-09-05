// dhello -- the first myrtos module written in D.
//
// It exists to prove the toolchain, so it uses the three things that could go
// wrong and nothing else: the trap (write), the command line (args), and a
// variable at module scope.
//
// That last one is the point of D being here at all. `calls` below is an
// ordinary module-level variable with no annotation, and D puts it in
// thread-local storage: the object comes out with .tbss and no .bss, so the
// module stays shared -- one copy in flash for every process running it.
//
// The same declaration in C is a writable static. That works too now, and costs
// the module a copy of itself per process to hold four bytes. In C the cheap
// way is __thread on each variable and remembering to write it; in D there is
// nothing to remember.
module dhello;

import myrtos;

@nogc: nothrow:

// No __gshared, so this is per-process. Two shells running dhello at once each
// count their own.
uint calls;

// argc and argv, exactly as a C module receives them -- the kernel builds the
// vector and enters here. (myrtos.args() exists too, but it stops at the first
// NUL, and by the time a module runs the kernel has split the argument string
// in place to build argv. It gives the first word and nothing after it.)
extern(C) void module_main(int argc, char** argv) {
    calls++;

    write(STDOUT, "hello from D\r\n");

    if (argc > 1) {
        write(STDOUT, "arguments:");
        foreach (i; 1 .. argc) {
            write(STDOUT, " ");
            write(STDOUT, argv[i], cast(uint) strlen(argv[i]));
        }
        write(STDOUT, "\r\n");
    }
}
