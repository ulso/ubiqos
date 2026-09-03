#include "../../common/myrtos_abi.h"
#include "wasm3.h"
#include "m3_env.h"      // for looking inside the module while this is being chased

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

// An M3Result is a const char*, and printing it is what actually crashed the
// board. m3_LoadModule returned an error, this walked the string to say which,
// and the load faulted: mcause 5 at lbu a5,0(a3), inside here. The real problem
// was the failed load; the crash was the reporting of it.
//
// So the pointer is shown before it is trusted, and only dereferenced if it
// points somewhere this module owns.
static bool result_readable(M3Result r)
{
    extern char __bss_start[], _end[];
    unsigned long a = (unsigned long)r;
    if (a >= MYRTOS_SINGLE_BASE && a < MYRTOS_SINGLE_BASE + MYRTOS_SINGLE_RESERVE) return true;
    if (a >= (unsigned long)__bss_start && a < (unsigned long)_end) return true;
    // And the heap: wasm3 copies export names and formats error messages there.
    extern char *wasm_heap_extent(unsigned long *size);
    unsigned long n = 0;
    char *h = wasm_heap_extent(&n);
    if (h && a >= (unsigned long)h && a < (unsigned long)h + n) return true;
    return false;
}

// Where wasm3 threw, which is more use than the result pointer: it records the
// file and line of the throw site.
static void report_error_site(IM3Runtime rt)
{
    M3ErrorInfo info;
    m3_GetErrorInfo(rt, &info);
    if (!info.line && !info.file) return;

    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "  thrown at line ");
    myrtos_line_u32(&l, info.line);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);
    if (result_readable(info.message)) say("  ", info.message);
}

static void fail(const char *what, M3Result r)
{
    myrtos_line_t l;
    myrtos_line_reset(&l);
    myrtos_line_str(&l, "wasm: ");
    myrtos_line_str(&l, what);
    myrtos_line_str(&l, " failed, M3Result 0x");
    myrtos_line_hex(&l, (uint32_t)(unsigned long)r);
    myrtos_line_str(&l, "\n");
    myrtos_line_flush(MYRTOS_STDOUT, &l);

    if (r && result_readable(r))  say("  ", r);
    else if (r)                   say("  ", "(pointer is not in this module)");
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

// WHERE THIS STANDS, 3 Sep 2026. It no longer crashes the board, and it does
// not yet run a function.
//
//     wasm: parsed
//     wasm: loaded
//     wasm: find run failed, M3Result 0x00010000
//
// So wasm3 starts, takes a heap from PSRAM, parses the embedded module and
// loads it. m3_FindFunction then fails, and what it returns is not one of
// wasm3's error constants -- those are in this module's .data around
// 0x117b02xx -- nor a pointer into the heap, which is in PSRAM below
// 0x11780000. 0x00010000 is neither. That is the next thread to pull.
//
// The crashes before this were not wasm3 at all. They were this file's own
// error reporting: an M3Result is a const char*, fail() walked it to say what
// went wrong, and the load faulted. Three power cycles went on hypotheses --
// the stack, then a bigger stack, then a third large frame that does not exist
// -- before the probe was used, and the probe answered in one go.
//
// HOW TO READ A CRASH HERE, since it took four attempts to learn:
//
//   A fatal trap never reaches the screen. myrtos_print puts the text in the
//   console ring and the task that draws it never runs again, so the machine
//   parks with the diagnosis written and undrawn. It is still in dmesg_buf:
//
//     nm build/os_kernel.elf | grep dmesg_buf
//     JLinkExe -device RP2350_RV32_0 -if SWD -autoconnect 1 -NoGui 1
//     > mem8 <that address> 2048
//
//   The handler also writes to the UART on GP44, which shows it live.
//
//   And mepc must be looked up in the exact ELF that was flashed. build/flashed
//   holds a copy for that reason; looking it up in a rebuilt one gave a
//   confident wrong answer once already.
//
// What was learned and is worth keeping: d_m3PreferStaticAlloc belongs ON for a
// SINGLE module, because mem_size is capped at 65536 and M3Compilation is
// 40592 bytes on its own; m3_info.c and m3_api_libc.c stay out of the build,
// being the only parts of wasm3 that reach newlib's stdio, which has no C
// startup here to initialise it.

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
    if (wasm_heap_init(4096u * 1024u) != 0) {
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
    if (r) { fail("load", r); report_error_site(runtime); goto free_rt; }

    MARK("loaded");

    // The imports the program asked for, before anything of it runs.
    extern M3Result wasm_link_wasi(IM3Module module);
    r = wasm_link_wasi(module);
    if (r) { fail("link wasi", r); goto free_rt; }

    MARK("wasi linked");

    // _start is what a WASI program is entered at; its main runs underneath.
    IM3Function f;
    r = m3_FindFunction(&f, runtime, "_start");
    if (r) { fail("find _start", r); report_error_site(runtime); goto free_rt; }

    MARK("running");
    r = m3_CallV(f);

    // Exiting is a trap by design -- proc_exit cannot return -- so the one the
    // program asks for is not a failure.
    extern bool wasm_exited;
    extern uint32_t wasm_exit_code;
    if (r && !wasm_exited) { fail("run", r); report_error_site(runtime); }
    else say_num("wasm: exited with ", (int32_t)wasm_exit_code);

free_rt:
    m3_FreeRuntime(runtime);
free_env:
    m3_FreeEnvironment(env);
}
