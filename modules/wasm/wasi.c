// The WASI calls a wasm program actually reaches for, and no more.
//
// The choice this represents: a program could be given myrtos-shaped imports
// and would then need myrtos-shaped glue in whatever language wrote it. Given
// WASI instead, an ordinary Rust program's println!, a Go program's
// fmt.Println and a C program's printf work unchanged, because that is what
// their standard libraries call underneath.
//
// It is a small subset on purpose. wasm3's full WASI is 1580 lines and forty
// functions; a Rust hello-world imports four of them, and this is those four.
// The rest are added when something asks for them, which is also how we will
// find out what is actually used.
//
// The fit is better than it sounds: WASI speaks in file descriptors, which is
// what myrtos speaks. fd_write is myrtos_write. When path_open arrives it will
// be myrtos_io_open, and /dev/acm -- the BleuIO -- is reachable through it like
// any other name.

#include "../../common/myrtos_abi.h"
#include "wasm3.h"
#include "m3_env.h"

// A wasm program's pointers are offsets into its own linear memory, so every
// one has to be translated before it is touched. m3ApiOffsetToPtr does that,
// and m3ApiGetArgMem does it while reading an argument.
typedef struct {
    uint32_t buf;      // an offset, not a pointer
    uint32_t len;
} wasi_iovec_t;

#define WASI_OK      0
#define WASI_EBADF   8
#define WASI_EFAULT  21

m3ApiRawFunction(wasi_fd_write)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t             , fd)
    m3ApiGetArgMem   (const wasi_iovec_t * , iovs)
    m3ApiGetArg      (uint32_t             , iovs_len)
    m3ApiGetArgMem   (uint32_t *           , nwritten)

    m3ApiCheckMem(iovs, iovs_len * sizeof(wasi_iovec_t));

    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t off = m3ApiReadMem32(&iovs[i].buf);
        uint32_t len = m3ApiReadMem32(&iovs[i].len);
        if (!len) continue;

        void *p = m3ApiOffsetToPtr(off);
        m3ApiCheckMem(p, len);

        int32_t n = myrtos_write((int32_t)fd, p, len);
        if (n < 0) m3ApiReturn(WASI_EBADF);
        total += (uint32_t)n;
    }

    m3ApiCheckMem(nwritten, sizeof(uint32_t));
    m3ApiWriteMem32(nwritten, total);
    m3ApiReturn(WASI_OK);
}

// No environment. Saying so plainly is what lets a runtime start: it asks for
// the sizes first and allocates nothing when they are zero.
m3ApiRawFunction(wasi_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t * , count)
    m3ApiGetArgMem   (uint32_t * , buf_size)

    m3ApiCheckMem(count, sizeof(uint32_t));
    m3ApiCheckMem(buf_size, sizeof(uint32_t));
    m3ApiWriteMem32(count, 0);
    m3ApiWriteMem32(buf_size, 0);
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_environ_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiReturn(WASI_OK);
}

// The program is done. Trapping out is how wasm3 unwinds an exit: returning
// normally would carry on executing after main.
uint32_t wasm_exit_code;
bool     wasm_exited;

m3ApiRawFunction(wasi_proc_exit)
{
    m3ApiGetArg(uint32_t, code);
    wasm_exit_code = code;

    wasm_exited = true;
    m3ApiTrap(m3Err_trapExit);
}

M3Result wasm_link_wasi(IM3Module module)
{
    static const char *ns = "wasi_snapshot_preview1";
    M3Result r;

    r = m3_LinkRawFunction(module, ns, "fd_write",          "i(i*i*)", &wasi_fd_write);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "environ_sizes_get", "i(**)",   &wasi_environ_sizes_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "environ_get",       "i(**)",   &wasi_environ_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "proc_exit",         "v(i)",    &wasi_proc_exit);
    if (r && r != m3Err_functionLookupFailed) return r;

    return m3Err_none;
}
