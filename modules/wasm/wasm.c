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

void module_main(void)
{
    myrtos_write_str(MYRTOS_STDOUT, "wasm: host built; nothing to run yet\n");

    // newlib's malloc has nowhere to grow until this is done. 192 kB from the
    // pool, which for a module that is not real-time means PSRAM.
    extern int wasm_heap_init(uint32_t bytes);
    if (wasm_heap_init(192u * 1024u) != 0) {
        myrtos_write_str(MYRTOS_STDOUT, "wasm: no room for a heap\n");
        return;
    }

    IM3Environment env = m3_NewEnvironment();
    if (!env) {
        myrtos_write_str(MYRTOS_STDOUT, "wasm: no environment\n");
        return;
    }

    IM3Runtime runtime = m3_NewRuntime(env, 8192, NULL);
    if (!runtime) {
        myrtos_write_str(MYRTOS_STDOUT, "wasm: no runtime\n");
        m3_FreeEnvironment(env);
        return;
    }

    myrtos_write_str(MYRTOS_STDOUT, "wasm: environment and runtime created\n");

    m3_FreeRuntime(runtime);
    m3_FreeEnvironment(env);
}
