#include "../../common/myrtos_abi.h"
#include <stdio.h>          // newlib's, which the port layer feeds
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

extern const unsigned char hello_wasm[];
extern const unsigned int  hello_wasm_len;

static void fail(const char *what, M3Result r)
{
    printf("wasm: %s: %s\n", what, r ? r : "(no reason given)");
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

// WHERE THIS STANDS, 3 Sep 2026: it crashes the board and the fault is not
// found. Read this before running it.
//
// It gets a long way. On the board it clears its bss, takes a heap from PSRAM,
// creates an environment and a runtime, and parses the embedded module. Then it
// dies, taking USB with it -- no console, no crash message, nothing to read.
//
// The interesting fact is that the point of death MOVES when memory sizes
// change. With a 192 kB heap and d_m3MaxFunctionStackHeight at 8000 it died in
// m3_FindFunction, one step after m3_LoadModule; with a 1 MB heap and the
// height at 1024 it dies in m3_LoadModule itself. That is the signature of
// something being written where it should not be, not of a clean allocation
// failure -- an out-of-memory would fail in the same place every time.
//
// Three hypotheses were wrong before that, and each cost a power cycle:
//   - the default 4 kB mem_size was too small (true, but not the whole story)
//   - 64 kB would be enough (no: mem_size is capped at 65536 and one of wasm3's
//     structures was 40 kB, which is why d_m3PreferStaticAlloc went back on)
//   - a third 40 kB frame in the compile path (no: CompileFunction uses
//     &runtime->compilation, which is in the malloc'd runtime)
//
// The next step is not another guess. The debug probe can read myrtos_crash,
// which the trap handler fills in with the cause and the address before the
// machine parks, and that turns this from hypotheses into a fact.

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
    printf("wasm: run() = %ld\n", (long)value);

    r = m3_FindFunction(&f, runtime, "add");
    if (!r) r = m3_CallV(f, 3, 4);
    if (!r) r = m3_GetResultsV(f, &value);
    if (r) fail("add", r);
    else   printf("wasm: add(3,4) = %ld\n", (long)value);

free_rt:
    m3_FreeRuntime(runtime);
free_env:
    m3_FreeEnvironment(env);
}
