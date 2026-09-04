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

// --- FILES, WHICH ARE ALSO DEVICES ----------------------------------------
//
// WASI opens a path relative to a preopened directory: a runtime asks
// fd_prestat_get about descriptors from 3 upwards until one says EBADF, and
// resolves every path against what it found. So one preopen is offered, and it
// is "/" -- the whole myrtos namespace. A program then opens /sd/notes.txt and
// /dev/acm by the same call, which is the point: to a wasm program the BleuIO
// is a file, and so is an FTDI dongle the day its driver registers one.
#define WASI_PREOPEN_FD    3
#define WASI_PREOPENTYPE_DIR 0

#define WASI_ENOENT   44
#define WASI_ENOTDIR  54
#define WASI_EINVAL   28

// The flags WASI states, and what myrtos calls the same things.
#define WASI_O_CREAT     0x0001
#define WASI_O_DIRECTORY 0x0002
#define WASI_O_EXCL      0x0004
#define WASI_O_TRUNC     0x0008
#define WASI_FDFLAG_APPEND 0x0001

#define WASI_RIGHT_FD_WRITE 0x0000000000000040ULL

// What each descriptor was opened as. fd_filestat_get is asked how long a file
// is and has only a number to go on, while the only thing here that can answer
// -- myrtos_fs_stat -- wants a name. So the name is kept when it is known.
// Sixteen is more open files than a wasm program has any business holding.
#define WASI_MAX_TRACKED 16

static struct { int32_t fd; char name[48]; } wasi_paths[WASI_MAX_TRACKED];

static void wasi_remember(int32_t fd, const char *name)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++) {
        if (wasi_paths[i].fd && wasi_paths[i].fd != fd) continue;
        wasi_paths[i].fd = fd;
        uint32_t n = 0;
        while (name[n] && n < sizeof(wasi_paths[i].name) - 1) { wasi_paths[i].name[n] = name[n]; n++; }
        wasi_paths[i].name[n] = 0;
        return;
    }
}

static const char *wasi_name_of(int32_t fd)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd == fd) return wasi_paths[i].name;
    return 0;
}

static void wasi_forget(int32_t fd)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd == fd) wasi_paths[i].fd = 0;
}

m3ApiRawFunction(wasi_fd_prestat_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t   , fd)
    m3ApiGetArgMem  (uint8_t *  , prestat)

    if (fd != WASI_PREOPEN_FD) m3ApiReturn(WASI_EBADF);

    // { u8 tag; u32 name_len; } with the length at offset 4 after padding.
    m3ApiCheckMem(prestat, 8);
    m3ApiWriteMem32(prestat, WASI_PREOPENTYPE_DIR);
    m3ApiWriteMem32(prestat + 4, 1);          // strlen("/")
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_fd_prestat_dir_name)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t , fd)
    m3ApiGetArgMem  (char *   , path)
    m3ApiGetArg     (uint32_t , path_len)

    if (fd != WASI_PREOPEN_FD) m3ApiReturn(WASI_EBADF);
    if (path_len < 1)          m3ApiReturn(WASI_EINVAL);

    m3ApiCheckMem(path, 1);
    path[0] = '/';
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_path_open)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t   , dirfd)
    m3ApiGetArg     (uint32_t   , dirflags)
    m3ApiGetArgMem  (const char*, path)
    m3ApiGetArg     (uint32_t   , path_len)
    m3ApiGetArg     (uint32_t   , oflags)
    m3ApiGetArg     (uint64_t   , rights_base)
    m3ApiGetArg     (uint64_t   , rights_inheriting)
    m3ApiGetArg     (uint32_t   , fdflags)
    m3ApiGetArgMem  (uint32_t * , out_fd)

    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(out_fd, sizeof(uint32_t));

    if (dirfd != WASI_PREOPEN_FD) m3ApiReturn(WASI_EBADF);

    // The name arrives without a terminator and relative to the preopen, which
    // is the root -- so it is put back together here rather than trusted.
    char name[80];
    if (path_len + 2 > sizeof(name)) m3ApiReturn(WASI_EINVAL);
    name[0] = '/';
    for (uint32_t i = 0; i < path_len; i++) name[1 + i] = path[i];
    name[1 + path_len] = 0;

    uint32_t flags = (rights_base & WASI_RIGHT_FD_WRITE) ? MYRTOS_O_RDWR : MYRTOS_O_RDONLY;
    if (oflags  & WASI_O_CREAT)      flags |= MYRTOS_O_CREAT;
    if (oflags  & WASI_O_TRUNC)      flags |= MYRTOS_O_TRUNC;
    if (fdflags & WASI_FDFLAG_APPEND) flags |= MYRTOS_O_APPEND;

    int32_t fd = myrtos_open_flags(name, flags);
    if (fd < 0) m3ApiReturn(WASI_ENOENT);

    wasi_remember(fd, name);
    m3ApiWriteMem32(out_fd, (uint32_t)fd);
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_fd_read)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t             , fd)
    m3ApiGetArgMem  (const wasi_iovec_t * , iovs)
    m3ApiGetArg     (uint32_t             , iovs_len)
    m3ApiGetArgMem  (uint32_t *           , nread)

    m3ApiCheckMem(iovs, iovs_len * sizeof(wasi_iovec_t));

    uint32_t total = 0;
    for (uint32_t i = 0; i < iovs_len; i++) {
        uint32_t off = m3ApiReadMem32(&iovs[i].buf);
        uint32_t len = m3ApiReadMem32(&iovs[i].len);
        if (!len) continue;

        void *p = m3ApiOffsetToPtr(off);
        m3ApiCheckMem(p, len);

        int32_t n = myrtos_read((int32_t)fd, p, len);
        if (n < 0) m3ApiReturn(WASI_EBADF);
        total += (uint32_t)n;
        if ((uint32_t)n < len) break;         // short read is the end of it
    }

    m3ApiCheckMem(nread, sizeof(uint32_t));
    m3ApiWriteMem32(nread, total);
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_fd_close)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t, fd)
    wasi_forget((int32_t)fd);
    m3ApiReturn(myrtos_close((int32_t)fd) < 0 ? WASI_EBADF : WASI_OK);
}

m3ApiRawFunction(wasi_fd_seek)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t  , fd)
    m3ApiGetArg     (int64_t   , offset)
    m3ApiGetArg     (uint32_t  , whence)
    m3ApiGetArgMem  (uint64_t *, out_pos)

    int32_t pos = myrtos_seek((int32_t)fd, (int32_t)offset, (int32_t)whence);
    if (pos < 0) m3ApiReturn(WASI_EBADF);

    m3ApiCheckMem(out_pos, sizeof(uint64_t));
    m3ApiWriteMem64(out_pos, (uint64_t)(uint32_t)pos);
    m3ApiReturn(WASI_OK);
}

// Enough of it to satisfy a runtime asking what kind of thing a descriptor is.
// Everything here is a character device as far as this says, which is true of
// the console and the dongle and a useful lie about a file until seeking needs
// to be advertised.
m3ApiRawFunction(wasi_fd_fdstat_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t  , fd)
    m3ApiGetArgMem  (uint8_t * , stat)

    m3ApiCheckMem(stat, 24);
    for (uint32_t i = 0; i < 24; i++) stat[i] = 0;
    stat[0] = (fd == WASI_PREOPEN_FD) ? 3 : 2;   // directory, or character device
    m3ApiReturn(WASI_OK);
}

// The size of what a descriptor is open on, which is what read_to_string asks
// before it allocates. There is no fstat here, so it is found the old way:
// seek to the end, note where that is, and seek back. A device answers zero and
// that is correct -- a stream has no length.
m3ApiRawFunction(wasi_fd_filestat_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t  , fd)
    m3ApiGetArgMem  (uint8_t * , buf)

    m3ApiCheckMem(buf, 64);
    for (uint32_t i = 0; i < 64; i++) buf[i] = 0;

    // By name, because that is what can be asked. A descriptor whose name is
    // not known -- the console, or one this did not open -- answers zero, and
    // for a stream that is the truth rather than a failure.
    uint32_t size = 0;
    const char *name = wasi_name_of((int32_t)fd);
    if (name) myrtos_fs_stat(name, &size);

    // { dev u64, ino u64, filetype u8, nlink u64, size u64, ... }
    buf[16] = 4;                       // regular file
    m3ApiWriteMem64(buf + 32, (uint64_t)size);
    m3ApiReturn(WASI_OK);
}

// --- TIME, AND WAITING ----------------------------------------------------
// A tick is a millisecond and that is the whole of the clock here. There is no
// calendar on this machine -- nothing sets a date and no battery keeps one --
// so the realtime clock and the monotonic clock are the same count of
// milliseconds since the board came up. A program that prints a timestamp will
// print one measured from boot, which is honest and is what the machine knows.
#define WASI_EINVAL 28

m3ApiRawFunction(wasi_clock_time_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArg      (uint32_t  , id)
    m3ApiGetArg      (uint64_t  , precision)
    m3ApiGetArgMem   (uint64_t *, out)

    m3ApiCheckMem(out, sizeof(uint64_t));
    (void)id; (void)precision;

    m3ApiWriteMem64(out, (uint64_t)myrtos_ticks_now() * 1000000ull);
    m3ApiReturn(WASI_OK);
}

// poll_oneoff is WASI's only way to wait, and sleep() is what compiles to it:
// one subscription on a clock, with a relative timeout. That is the case worth
// implementing properly, and it is the case the BleuIO needed -- the dongle
// wants a pause between AT+CENTRAL and the scan that follows it, and without
// one the setup goes out faster than the chip can act on it.
//
// The structures are the fiddly part, so they are spelled out rather than
// mapped onto C types: a subscription is 48 bytes with the union at 16, and an
// event is 32. Everything is little endian and read a field at a time, because
// the guest's memory is not aligned the way this side would like.
//
//   subscription   userdata u64 @0, tag u8 @8,
//                  clock: id u32 @16, timeout u64 @24, precision u64 @32,
//                         flags u16 @40   (bit 0 set = the timeout is absolute)
//   event          userdata u64 @0, error u16 @8, type u8 @10
//
// A subscription on a file descriptor is answered as ready without looking.
// That is not a fudge: a read here blocks until there is something, so a
// program that polls and then reads gets exactly what it would have got, and
// one that polls several descriptors at once would need machinery this host
// does not have. Nothing has asked for it yet.
#define WASI_SUB_SIZE   48u
#define WASI_EVENT_SIZE 32u
#define WASI_EVENTTYPE_CLOCK 0u

m3ApiRawFunction(wasi_poll_oneoff)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (const uint8_t *, in)
    m3ApiGetArgMem   (uint8_t *      , out)
    m3ApiGetArg      (uint32_t       , nsubs)
    m3ApiGetArgMem   (uint32_t *     , nevents)

    if (!nsubs) { m3ApiWriteMem32(nevents, 0); m3ApiReturn(WASI_EINVAL); }
    m3ApiCheckMem(in,  nsubs * WASI_SUB_SIZE);
    m3ApiCheckMem(out, nsubs * WASI_EVENT_SIZE);
    m3ApiCheckMem(nevents, sizeof(uint32_t));

    // The longest clock timeout among the subscriptions is not what to wait
    // for -- the SHORTEST is, because poll returns when the first of them is
    // ready. A single sleep has one and the distinction does not arise, but
    // getting it backwards would turn a 200 ms pause into whatever else was
    // being waited on.
    uint64_t wait_ns = 0;
    bool have_clock = false, ready_now = false;

    for (uint32_t i = 0; i < nsubs; i++) {
        const uint8_t *sub = in + i * WASI_SUB_SIZE;
        if (sub[8] != 0) { ready_now = true; continue; }      // a descriptor: ready

        uint64_t timeout = m3ApiReadMem64(sub + 24);
        uint16_t flags   = m3ApiReadMem16(sub + 40);
        if (flags & 1u) {                                     // absolute, so subtract now
            uint64_t now = (uint64_t)myrtos_ticks_now() * 1000000ull;
            timeout = (timeout > now) ? timeout - now : 0;
        }
        if (!have_clock || timeout < wait_ns) wait_ns = timeout;
        have_clock = true;
    }

    // A descriptor that is ready already means there is nothing to wait for.
    if (have_clock && !ready_now && wait_ns) {
        uint32_t ms = (uint32_t)(wait_ns / 1000000ull);
        // Anything under a millisecond still yields: a program asking for a
        // pause wants the processor to go elsewhere, however short the pause.
        myrtos_sleep(ms ? ms : 1);
    }

    // One event per subscription, all of them reporting success. Zero the whole
    // structure first: the fields this host does not fill are read by the guest
    // regardless, and whatever was in its memory would be read as an error.
    for (uint32_t i = 0; i < nsubs; i++) {
        const uint8_t *sub = in  + i * WASI_SUB_SIZE;
        uint8_t       *ev  = out + i * WASI_EVENT_SIZE;
        for (uint32_t b = 0; b < WASI_EVENT_SIZE; b++) ev[b] = 0;
        m3ApiWriteMem64(ev, m3ApiReadMem64(sub));             // userdata, echoed back
        m3ApiWriteMem16(ev + 8, 0);                           // error: none
        ev[10] = sub[8];                                      // the type asked for
    }
    m3ApiWriteMem32(nevents, nsubs);
    m3ApiReturn(WASI_OK);
}

// --- ARGUMENTS AND RANDOMNESS ---------------------------------------------
// The program's own arguments, so "wasm /sd/prog.wasm one two" reaches the
// program as argv. Without these a guest gets nothing and has to hardcode every
// path -- which is what the examples had to do until now.
//
// argv[0] is the program, as everywhere since Unix, and the rest is whatever
// followed it on the command line. The host module hands them over before the
// guest starts; see wasm_set_args in wasm.c.
static const char  *arg_prog = "program";
static int          arg_extra;
static char       **arg_list;

void wasm_set_args(const char *prog, int argc, char **argv)
{
    if (prog && prog[0]) arg_prog = prog;
    arg_extra = (argc > 0) ? argc : 0;
    arg_list  = argv;
}

static const char *arg_at(uint32_t i) { return i ? arg_list[i - 1] : arg_prog; }

static uint32_t arg_len(uint32_t i)
{
    const char *s = arg_at(i);
    uint32_t n = 0;
    while (s[n]) n++;
    return n;
}

static uint32_t arg_count(void) { return 1u + (uint32_t)arg_extra; }

static uint32_t arg_bytes(void)
{
    uint32_t total = 0;
    for (uint32_t i = 0; i < arg_count(); i++) total += arg_len(i) + 1;
    return total;
}

m3ApiRawFunction(wasi_args_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t * , count)
    m3ApiGetArgMem   (uint32_t * , buf_size)

    m3ApiCheckMem(count, sizeof(uint32_t));
    m3ApiCheckMem(buf_size, sizeof(uint32_t));
    m3ApiWriteMem32(count, arg_count());
    m3ApiWriteMem32(buf_size, arg_bytes());
    m3ApiReturn(WASI_OK);
}

// The vector holds OFFSETS into the guest's memory, not host pointers, so each
// one is converted back on the way in. Getting that wrong gives a program an
// argv full of addresses from this side of the sandbox.
m3ApiRawFunction(wasi_args_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t * , argv)
    m3ApiGetArgMem   (char *     , buf)

    m3ApiCheckMem(argv, arg_count() * sizeof(uint32_t));
    m3ApiCheckMem(buf, arg_bytes());

    char *w = buf;
    for (uint32_t i = 0; i < arg_count(); i++) {
        m3ApiWriteMem32(&argv[i], m3ApiPtrToOffset(w));
        const char *s = arg_at(i);
        uint32_t n = arg_len(i);
        for (uint32_t k = 0; k < n; k++) *w++ = s[k];
        *w++ = 0;
    }
    m3ApiReturn(WASI_OK);
}

// Rust's standard library asks for this before main runs: its hash maps are
// seeded from it, so a program that never mentions randomness still needs it to
// start. The kernel reads the ring oscillator's random bit and mixes in the
// microsecond timer -- entropy enough to seed a hash, and not a key.
//
// It fills at most 256 bytes a call, because the call is a trap and a trap runs
// with interrupts off, so this loops rather than asking for everything at once.
m3ApiRawFunction(wasi_random_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint8_t * , buf)
    m3ApiGetArg      (uint32_t  , len)

    m3ApiCheckMem(buf, len);

    uint32_t done = 0;
    while (done < len) {
        int32_t n = myrtos_random(buf + done, len - done);
        if (n <= 0) m3ApiReturn(WASI_EINVAL);
        done += (uint32_t)n;
    }
    m3ApiReturn(WASI_OK);
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

    r = m3_LinkRawFunction(module, ns, "fd_prestat_get",      "i(i*)",      &wasi_fd_prestat_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_prestat_dir_name", "i(i*i)",     &wasi_fd_prestat_dir_name);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "path_open",           "i(ii*iiIIi*)", &wasi_path_open);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_read",             "i(i*i*)",    &wasi_fd_read);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_close",            "i(i)",       &wasi_fd_close);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_seek",             "i(iIi*)",    &wasi_fd_seek);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_fdstat_get",       "i(i*)",      &wasi_fd_fdstat_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_filestat_get",     "i(i*)",      &wasi_fd_filestat_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "clock_time_get",       "i(iI*)",     &wasi_clock_time_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "poll_oneoff",          "i(**i*)",    &wasi_poll_oneoff);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "args_sizes_get",       "i(**)",      &wasi_args_sizes_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "args_get",             "i(**)",      &wasi_args_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "random_get",           "i(*i)",      &wasi_random_get);
    if (r && r != m3Err_functionLookupFailed) return r;

    return m3Err_none;
}
