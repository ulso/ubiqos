#include "../../common/myrtos_abi.h"
#include "wasm3.h"

// wasm -- a WebAssembly host, so an application can be written in any language
// with a wasm backend and shipped as a file rather than as a myrtos module.
//
// This is the reason it exists. Everything docs/writing-modules.md teaches --
// no writable statics, no tables of pointers, __thread on every variable that
// needs to be per-process -- is the price of running native code from flash on
// a machine with no MMU. A wasm module has its own linear memory and its own
// globals, so none of that reaches the program. What the machine cannot provide
// with hardware, the sandbox provides with an interpreter.
//
// SINGLE, and not by choice. wasm3's interpreter is built out of tables of
// function pointers -- M3OpInfo holds four IM3Operation each, for the whole
// instruction set -- and a table of pointers is exactly what a shareable module
// may not contain. Rebuilding those as indices would be rewriting the heart of
// the project. Marked SINGLE it links at a fixed address in PSRAM where
// absolute addresses are correct, and may use newlib as it stands.
//
// One instance, therefore. That is not the limitation it sounds like: wasm3 is
// built to host several modules in one runtime, with IM3Runtime as the per
// program handle, so several wasm programs share one host process and are
// isolated from each other inside it rather than by myrtos.
//
// Measured before any of this was written: the twelve source files compile for
// rv32 with myrtos's own flags without a change, and come to 122 kB of code
// with .data and .bss both empty -- wasm3 has no writable globals in this
// configuration. Linked against newlib it is about 193 kB against the 512 kB
// the SINGLE region reserves.

// Thirty-two kilobytes, which is the stack the interpreter needs and no more.
//
// The two big frames are not on it, and getting that right took two crashes.
// wasm3 has a flag, d_m3PreferStaticAlloc, that keeps M3Compilation and
// M3Runtime as function-scope statics instead of locals -- and they are not
// small: 40592 and 40976 bytes, because both hold arrays sized by
// d_m3MaxFunctionStackHeight, two thousand entries by default. The flag exists
// because on a small microcontroller you cannot put forty kilobytes on a stack.
//
// I turned it off out of habit from the shareable world, where a writable
// static is what makes a module unshareable. This module is SINGLE, so it may
// have them, and turning the flag back on is free. The first attempt ran with
// the default 4096 and crashed hard enough to take USB with it; the second
// tried 64 kB, still less than one frame; and mem_size is capped at 65536 by
// the module format anyway, so the stack was never the way out.
MYRTOS_MEM_SIZE(32 * 1024);

// m3_info.c is left out of the build: it is the debug and tracing half of
// wasm3 and the only part that reaches newlib's stdio, which cannot work here.
// One symbol from it is referenced unconditionally, so it gets a body.
void m3_PrintProfilerInfo(void) { }

extern const unsigned char hello_wasm[];
extern const unsigned int  hello_wasm_len;

// Not printf. newlib's stdio needs an initialised reent structure and there is
// no C startup here to build one, so _impure_ptr points at nothing and the
// first call reads a FILE at a misaligned address. That is what crashed the
// board three times: mcause 4 at lh a5,12(s1) inside _vfprintf_r, with s1 odd.
//
// myrtos_write_str is the kernel's own path and needs nothing set up.
static void say(const char *a, const char *b)
{
    myrtos_write_str(MYRTOS_STDOUT, a);
    if (b) myrtos_write_str(MYRTOS_STDOUT, b);
    myrtos_write_str(MYRTOS_STDOUT, "\n");
}

static void say_num(const char *a, int32_t v)
{
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, a);
    if (v < 0) { myrtos_line_str(&l, "-"); v = -v; }
    myrtos_line_u32(&l, (uint32_t)v);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
}

static void fail(const char *what, M3Result r)
{
    myrtos_write_str(MYRTOS_STDOUT, "wasm: ");
    say(what, r ? ": " : " failed");
    if (r) say("  ", r);
}

// The module's own .bss, which nobody else will clear.
//
// objcopy makes the image from the loadable sections and drops .bss, and the
// loader copies module_size bytes -- so eighty kilobytes of supposedly zeroed
// data arrive as whatever was in PSRAM. It happens to work, because wasm3
// initialises both big structures before use, but that is luck and not a
// promise. The region is inside the SINGLE reserve, so zeroing it is simply
// what a C startup would have done had there been one.
extern char __bss_start[], _end[];

static void clear_bss(void)
{
    for (char *p = __bss_start; p < _end; p++)
        *p = 0;
}

// WHERE THIS STANDS, 3 Sep 2026: it crashes the board and the fault is found
// but not fixed. Read this before running it.
//
// It gets a long way: on the board it clears its bss, takes a heap from PSRAM,
// creates an environment and a runtime, and parses the embedded module. Then it
// takes a fatal trap and the machine parks in the trap handler's wfi loop.
//
// THE FAULT, read with the probe rather than guessed:
//
//     *** MYRTOS TRAP: unhandled exception ***
//       mepc 293223096  mcause 4  mtval 0
//
// mcause 4 is a misaligned load, and mepc is 0x117A3AB8 -- inside this module.
// It also explains why the point of death moved when memory sizes changed:
// whether a given access lands on an odd address depends on where things fell.
//
// HOW TO READ IT AGAIN, because this took four attempts to learn. A fatal trap
// never reaches the screen: myrtos_print puts the text in the console ring and
// the task that draws it never runs again. The text is still in dmesg_buf, so
//
//     nm os_kernel.elf | grep dmesg_buf
//     JLinkExe -device RP2350_RV32_0 -if SWD -autoconnect 1   then mem8 <addr> 2048
//
// gets it out. The trap handler also writes to the UART on GP44, which is the
// other way to see it live.
//
// AND A MISTAKE NOT TO REPEAT: mepc must be looked up in the exact ELF that was
// flashed. I looked it up in a rebuilt one and got a confident, wrong answer
// about which function it was.
//
// Three hypotheses were wrong before the probe was used, and each cost a power
// cycle: that the default 4 kB mem_size was the whole story; that 64 kB would
// do (mem_size is capped at 65536 and one wasm3 structure is 40 kB, which is
// why d_m3PreferStaticAlloc belongs ON for a SINGLE module); and that the
// compile path had a third large stack frame (it does not -- CompileFunction
// uses &runtime->compilation, which is in the malloc'd runtime).
//
// Since then m3_info.c and m3_api_libc.c are out of the build and this file no
// longer calls printf. newlib's stdio needs an initialised reent structure and
// there is no C startup here to build one, so it was a hazard whether or not it
// was the cause.

#define MARK(s) myrtos_write_str(MYRTOS_STDOUT, "wasm: " s "\n")

void module_main(void)
{
    MARK("entered");
    clear_bss();
    MARK("bss cleared");

    // newlib's malloc has nowhere to grow until this is done. 192 kB from the
    // pool, which for a module that is not real-time means PSRAM, where there are
    // nearly eight megabytes. A megabyte because the arithmetic said 192 kB was
    // tight and an unchecked malloc failure looks exactly like the crash we
    // have: m3_NewRuntime asks for 8192 stack slots, which is 64 kB on its own,
    // M3Runtime is another 41, and the compiler allocates code pages on top.
    extern int wasm_heap_init(uint32_t bytes);
    if (wasm_heap_init(1024u * 1024u) != 0) {
        myrtos_write_str(MYRTOS_STDOUT, "wasm: no room for a heap\n");
        return;
    }

    MARK("heap ready");
    IM3Environment env = m3_NewEnvironment();
    if (!env) { myrtos_write_str(MYRTOS_STDOUT, "wasm: no environment\n"); return; }

    MARK("environment");
    IM3Runtime runtime = m3_NewRuntime(env, 8192, NULL);
    if (!runtime) { myrtos_write_str(MYRTOS_STDOUT, "wasm: no runtime\n"); goto free_env; }

    MARK("runtime");
    IM3Module module;
    M3Result r = m3_ParseModule(env, &module, hello_wasm, hello_wasm_len);
    if (r) { fail("parse", r); goto free_rt; }

    MARK("parsed");
    r = m3_LoadModule(runtime, module);
    if (r) { fail("load", r); goto free_rt; }

    MARK("loaded");
    IM3Function f;
    r = m3_FindFunction(&f, runtime, "run");
    if (r) { fail("find run", r); goto free_rt; }

    MARK("found run");
    r = m3_CallV(f);
    if (r) { fail("call run", r); goto free_rt; }

    int32_t value = 0;
    r = m3_GetResultsV(f, &value);
    if (r) { fail("result", r); goto free_rt; }
    say_num("wasm: run() = ", value);

    r = m3_FindFunction(&f, runtime, "add");
    if (!r) r = m3_CallV(f, 3, 4);
    if (!r) r = m3_GetResultsV(f, &value);
    if (r) fail("add", r);
    else   say_num("wasm: add(3,4) = ", value);

free_rt:
    m3_FreeRuntime(runtime);
free_env:
    m3_FreeEnvironment(env);
}
