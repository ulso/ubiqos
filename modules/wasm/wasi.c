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
// --- SIXTY-FOUR BITS, BYTE BY BYTE ----------------------------------------
// Never store a 64-bit value into the guest's memory with one instruction.
//
// The guest may align its own structures perfectly and it still does not help:
// the host pointer is the memory base plus the guest's offset, and the base is
// only as aligned as whoever allocated it made it. Hazard3 does not do
// misaligned accesses -- it traps with mcause 6 -- so an eight-byte store to a
// four-byte-aligned address takes the machine down.
//
// It cost an evening. fd_readdir writes a dirent header whose first field is
// 64 bits, and a program that did nothing but list a directory killed the USB
// stack every time. The trap said mcause 6 and an address inside this module,
// which is exactly what it was.
static void wasi_put64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t wasi_get64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i);
    return v;
}

static void wasi_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i));
}


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

// The environment: the screen, and where the guest starts from.
//
// A wasm guest has no ioctl and no terminal to ask, so a curses program has no
// way to find out how big the console is -- Atto drew thirty lines of eighty
// columns on a screen of forty by a hundred and six, because that is what the
// shim had been told to assume. The size goes where a program already looks for
// it: LINES and COLUMNS. Nothing about that is particular to myrtos, which is
// the point.
#define WASI_ENV_COUNT 3

static char     wasi_env_buf[96];
static uint32_t wasi_env_off[WASI_ENV_COUNT];
static uint32_t wasi_env_len;

static uint32_t wasi_env_str(uint32_t at, const char *key, const char *value)
{
    for (const char *k = key; *k; k++) wasi_env_buf[at++] = *k;
    wasi_env_buf[at++] = '=';
    for (const char *v = value; *v; v++) wasi_env_buf[at++] = *v;
    wasi_env_buf[at++] = 0;
    return at;
}

static uint32_t wasi_env_put(uint32_t at, const char *key, uint32_t value)
{
    for (const char *k = key; *k; k++) wasi_env_buf[at++] = *k;
    wasi_env_buf[at++] = '=';

    char digits[8];
    uint32_t n = 0;
    do { digits[n++] = (char)('0' + value % 10); value /= 10; } while (value && n < sizeof digits);
    while (n) wasi_env_buf[at++] = digits[--n];

    wasi_env_buf[at++] = 0;
    return at;
}

// Asked again on every call, because the font can change under a running
// program and the grid changes with it.
static void wasi_env_prepare(void)
{
    myrtos_confont_t f;
    uint32_t rows = 30, cols = 80;
    if (myrtos_console_font_info(-1, &f) >= 0) { rows = f.rows; cols = f.cols; }

    // Where the guest starts from. wasi-libc begins at the preopen root and has
    // no way to be told otherwise, so a program that means to honour the
    // directory it was started in reads PWD and changes to it -- which is what
    // PWD is for. Without it "note.txt" means "/note.txt" whatever the shell's
    // cwd was, and saving under a bare name fails.
    char cwd[48];
    cwd[0] = '/'; cwd[1] = 0;
    myrtos_getcwd(cwd, sizeof cwd);
    if (!cwd[0]) { cwd[0] = '/'; cwd[1] = 0; }

    uint32_t at = 0;
    wasi_env_off[0] = at; at = wasi_env_put(at, "LINES", rows);
    wasi_env_off[1] = at; at = wasi_env_put(at, "COLUMNS", cols);
    wasi_env_off[2] = at; at = wasi_env_str(at, "PWD", cwd);
    wasi_env_len = at;
}

m3ApiRawFunction(wasi_environ_sizes_get)
{
    m3ApiReturnType  (uint32_t)
    m3ApiGetArgMem   (uint32_t * , count)
    m3ApiGetArgMem   (uint32_t * , buf_size)

    m3ApiCheckMem(count, sizeof(uint32_t));
    m3ApiCheckMem(buf_size, sizeof(uint32_t));
    wasi_env_prepare();
    wasi_put32((uint8_t *)count, WASI_ENV_COUNT);
    wasi_put32((uint8_t *)buf_size, wasi_env_len);
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_environ_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArgMem  (uint32_t *, env)
    m3ApiGetArgMem  (char *    , env_buf)

    wasi_env_prepare();
    m3ApiCheckMem(env, WASI_ENV_COUNT * sizeof(uint32_t));
    m3ApiCheckMem(env_buf, wasi_env_len);

    for (uint32_t i = 0; i < wasi_env_len; i++) env_buf[i] = wasi_env_buf[i];
    for (uint32_t i = 0; i < WASI_ENV_COUNT; i++)
        wasi_put32((uint8_t *)(env + i), m3ApiPtrToOffset(env_buf + wasi_env_off[i]));
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

static struct { int32_t fd; bool is_dir; bool doomed; char name[48]; } wasi_paths[WASI_MAX_TRACKED];

static void wasi_remember_kind(int32_t fd, const char *name, bool is_dir)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++) {
        if (wasi_paths[i].fd && wasi_paths[i].fd != fd) continue;
        wasi_paths[i].fd = fd;
        wasi_paths[i].is_dir = is_dir;
        wasi_paths[i].doomed = false;
        uint32_t n = 0;
        while (name[n] && n < sizeof(wasi_paths[i].name) - 1) { wasi_paths[i].name[n] = name[n]; n++; }
        wasi_paths[i].name[n] = 0;
        return;
    }
}

static void wasi_remember(int32_t fd, const char *name)
{
    wasi_remember_kind(fd, name, false);
}

static bool wasi_is_dir(int32_t fd)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd == fd) return wasi_paths[i].is_dir;
    return false;
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

static bool wasi_names_equal(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

// Unlinking a file that is still open. POSIX keeps such a file alive until the
// last descriptor closes, and programs lean on it harder than they look: Atto's
// completion opens its temp file, unlinks it immediately so that nothing is
// left behind however it exits, and only then reads the names back through the
// descriptor it kept. A FAT directory entry has nowhere to record "gone but
// still open" -- the entry is the file, and removing it makes every later read
// fail -- so the removal is held here until the descriptor closes. Measured on
// the board before it was written: a read through a descriptor opened before
// the write still works, and the same read after an unlink returns -1.
static bool wasi_doom(const char *name)
{
    bool held = false;
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd && wasi_names_equal(wasi_paths[i].name, name)) {
            wasi_paths[i].doomed = true;
            held = true;
        }
    return held;
}

// A program that exits without closing leaves its doomed names behind, since
// the close that was to carry them out never comes. The kernel reclaims the
// descriptors; this reclaims the files.
void wasi_sweep_doomed(void)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd && wasi_paths[i].doomed) {
            myrtos_fs_remove(wasi_paths[i].name);
            wasi_paths[i].fd = 0;
        }
}

static bool wasi_is_doomed(int32_t fd)
{
    for (int i = 0; i < WASI_MAX_TRACKED; i++)
        if (wasi_paths[i].fd == fd) return wasi_paths[i].doomed;
    return false;
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

// Where a guest's path actually points.
//
// A path arrives with the preopen stripped off, and the preopen is the root, so
// "note.txt" and "/note.txt" reach here as the same bytes -- no call in WASI
// tells them apart. wasi-libc keeps the working directory inside the guest,
// where the host has no way to set it, so the host decides here instead. That
// is why this is not left to each program: it cannot be.
//
// The rule is exact rather than a guess, because myrtos's root holds volumes
// and nothing else. The first component of an absolute path is always a volume
// name, so if it is one the path is absolute. If it is not, there is no such
// thing at the root and the path can only mean the directory the process is
// standing in -- which is what a shell would have decided.
static bool wasi_first_is_volume(const char *path, uint32_t len)
{
    char vol[MYRTOS_DIRNAME_MAX + 1];
    uint32_t n = 0;
    vol[n++] = '/';
    for (uint32_t i = 0; i < len && path[i] != '/'; i++) {
        if (n >= sizeof vol - 1) return false;
        vol[n++] = path[i];
    }
    vol[n] = 0;
    if (n == 1) return true;               // the root itself

    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(vol, &size);
    return attr >= 0 && (attr & MYRTOS_ATTR_DIRECTORY);
}

static bool wasi_path_name(const char *path, uint32_t len, char *out, uint32_t cap)
{
    // "./x" names the directory the process is standing in, and wasi-libc passes
    // it through untouched -- it has no working directory to fold it into -- so
    // it is folded away here and then resolved like any other bare name.
    while (len >= 2 && path[0] == '.' && path[1] == '/') { path += 2; len -= 2; }

    // A bare "." is the one name that cannot be honoured, and it is the root
    // that wins it. An absolute "/" arrives as "." as well: wasi-libc puts it
    // there because the *at calls take no empty path, and by then the two are
    // the same request. Asking for "/" and being given a directory that is not
    // the root is the worse of the two answers, so "." lists the volumes.
    if (len == 0 || (len == 1 && path[0] == '.')) {
        if (cap < 2) return false;
        out[0] = '/';
        out[1] = 0;
        return true;
    }

    if (!wasi_first_is_volume(path, len)) {
        char cwd[MYRTOS_DIRNAME_MAX + 1];
        cwd[0] = 0;
        myrtos_getcwd(cwd, sizeof cwd);
        if (cwd[0] && !(cwd[0] == '/' && !cwd[1])) {
            uint32_t n = 0;
            while (cwd[n]) { if (n >= cap - 1) return false; out[n] = cwd[n]; n++; }
            if (n + len + 2 > cap) return false;
            out[n++] = '/';
            for (uint32_t i = 0; i < len; i++) out[n + i] = path[i];
            out[n + len] = 0;
            return true;
        }
    }

    if (len + 2 > cap) return false;
    out[0] = '/';
    for (uint32_t i = 0; i < len; i++) out[1 + i] = path[i];
    out[1 + len] = 0;
    return true;
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

    // The name arrives without a terminator and with the preopen stripped off,
    // so it is put back together here rather than trusted -- and which
    // directory it is put back together against is wasi_path_name's business.
    char name[80];
    if (!wasi_path_name(path, path_len, name, sizeof name)) m3ApiReturn(WASI_EINVAL);

    // A directory is opened too, because a program that completes a filename
    // has to read one. myrtos has no directory-open of its own -- a directory is
    // a thing you list, not a thing you hold -- so /dev/null stands in as the
    // handle and the remembered name does the work. The descriptor is a real
    // one, which is what matters: fd_close closes it like any other, and nothing
    // downstream has to know it is special.
    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(name, &size);
    if (attr >= 0 && (attr & MYRTOS_ATTR_DIRECTORY)) {
        int32_t dfd = myrtos_open("/dev/null");
        if (dfd < 0) m3ApiReturn(WASI_EBADF);
        wasi_remember_kind(dfd, name, true);
        m3ApiWriteMem32(out_fd, (uint32_t)dfd);
        m3ApiReturn(WASI_OK);
    }

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

    // A name held back by an unlink is removed now, after the close, which is
    // the moment POSIX says the file stops existing.
    char victim[48];
    victim[0] = 0;
    if (wasi_is_doomed((int32_t)fd)) {
        const char *n = wasi_name_of((int32_t)fd);
        uint32_t i = 0;
        if (n) { while (n[i] && i < sizeof victim - 1) { victim[i] = n[i]; i++; } }
        victim[i] = 0;
    }

    wasi_forget((int32_t)fd);
    int32_t r = myrtos_close((int32_t)fd);
    if (victim[0]) myrtos_fs_remove(victim);
    m3ApiReturn(r < 0 ? WASI_EBADF : WASI_OK);
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
    wasi_put64((uint8_t *)out_pos, (uint64_t)(uint32_t)pos);
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
    stat[0] = (fd == WASI_PREOPEN_FD || wasi_is_dir((int32_t)fd)) ? 3 : 2;
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
    wasi_put64(buf + 32, (uint64_t)size);
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

    wasi_put64((uint8_t *)out, (uint64_t)myrtos_ticks_now() * 1000000ull);
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

        uint64_t timeout = wasi_get64(sub + 24);
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
        wasi_put64(ev, wasi_get64(sub));                      // userdata, echoed back
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

// --- BY NAME RATHER THAN BY DESCRIPTOR ------------------------------------
// A program that manages files rather than merely reading them needs these:
// Atto stats a file before opening it and unlinks the temporary one it makes
// for completion. Both go to the same myrtos calls the fd versions use; the
// only new work is putting the path back together, which path_open already
// does the same way -- the name arrives without a terminator and relative to
// the preopen, so the leading slash is added here.
m3ApiRawFunction(wasi_path_filestat_get)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t   , dirfd)
    m3ApiGetArg     (uint32_t   , flags)
    m3ApiGetArgMem  (const char*, path)
    m3ApiGetArg     (uint32_t   , path_len)
    m3ApiGetArgMem  (uint8_t *  , buf)

    (void)flags;
    m3ApiCheckMem(path, path_len);
    m3ApiCheckMem(buf, 64);
    if (dirfd != WASI_PREOPEN_FD) m3ApiReturn(WASI_EBADF);

    char name[80];
    if (!wasi_path_name(path, path_len, name, sizeof name)) m3ApiReturn(WASI_EINVAL);

    uint32_t size = 0;
    int32_t attr = myrtos_fs_stat(name, &size);
    if (attr < 0) m3ApiReturn(WASI_ENOENT);

    for (uint32_t i = 0; i < 64; i++) buf[i] = 0;
    // { dev u64, ino u64, filetype u8, nlink u64, size u64, ... }
    buf[16] = (attr & MYRTOS_ATTR_DIRECTORY) ? 3 : 4;   // directory, else regular
    wasi_put64(buf + 32, (uint64_t)size);
    m3ApiReturn(WASI_OK);
}

m3ApiRawFunction(wasi_path_unlink_file)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t   , dirfd)
    m3ApiGetArgMem  (const char*, path)
    m3ApiGetArg     (uint32_t   , path_len)

    m3ApiCheckMem(path, path_len);
    if (dirfd != WASI_PREOPEN_FD) m3ApiReturn(WASI_EBADF);

    char name[80];
    if (!wasi_path_name(path, path_len, name, sizeof name)) m3ApiReturn(WASI_EINVAL);
    if (wasi_doom(name)) m3ApiReturn(WASI_OK);
    if (myrtos_fs_remove(name) < 0) m3ApiReturn(WASI_ENOENT);
    m3ApiReturn(WASI_OK);
}

// Every descriptor here blocks, and there is no other mode to set. Answering
// yes to a program that asks for one is the right answer: it asked to be sure,
// and being told no would send a libc down a path that has no meaning here.
m3ApiRawFunction(wasi_fd_fdstat_set_flags)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t, fd)
    m3ApiGetArg     (uint32_t, flags)
    (void)fd; (void)flags;
    m3ApiReturn(WASI_OK);
}


// --- READING A DIRECTORY --------------------------------------------------
// The call a filename completion needs, and the last thing that stood between
// Atto's TAB and a list of names.
//
// The cookie is the entry number, which is exactly what myrtos_fs_dir_at wants,
// so the two agree without any bookkeeping in between. Each entry is a
// twenty-four byte header and then the name, packed one after another; a header
// that does not fit is simply not written, and a caller that gets less than it
// asked for knows to come back with the last cookie.
//
//   dirent { next u64 @0, ino u64 @8, namlen u32 @16, type u8 @20, pad[3] }
#define WASI_DIRENT_SIZE 24

// A directory entry needs an inode number, and a FAT directory has none to
// give. Zero is not the neutral answer it looks like: wasi-libc reads it as
// "unknown, go and find out", calls fstatat relative to the directory
// descriptor, and silently drops every entry whose lookup fails. A hash of the
// path is stable across a listing, distinct between directories, and never
// zero, which is all a caller can ask of it.
static uint64_t wasi_inode(const char *dir, const char *name)
{
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = dir; *p; p++)  { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    h ^= '/'; h *= 1099511628211ULL;
    for (const char *p = name; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    return h ? h : 1;
}



m3ApiRawFunction(wasi_fd_readdir)
{
    m3ApiReturnType (uint32_t)
    m3ApiGetArg     (uint32_t , fd)
    m3ApiGetArgMem  (uint8_t *, buf)
    m3ApiGetArg     (uint32_t , buf_len)
    m3ApiGetArg     (uint64_t , cookie)
    m3ApiGetArgMem  (uint32_t*, bufused)

    m3ApiCheckMem(buf, buf_len);
    m3ApiCheckMem(bufused, sizeof(uint32_t));

    // A tracked directory first, and the preopen only if the descriptor is not
    // one. The two collide: WASI_PREOPEN_FD is 3 and 3 is also the first
    // descriptor the kernel hands a guest, so asking about the preopen first
    // listed the root whatever directory had been opened. It is a collision
    // worth remembering -- fd_prestat_get answers for 3 as the preopen while
    // everything else may legitimately have 3 as a file.
    const char *dir = wasi_is_dir((int32_t)fd) ? wasi_name_of((int32_t)fd)
                    : (fd == WASI_PREOPEN_FD ? "/" : 0);
    if (!dir) {
        wasi_put32((uint8_t *)bufused, 0);
        m3ApiReturn(WASI_EBADF);
    }

    uint32_t used = 0;
    uint32_t index = (uint32_t)cookie;

    for (;;) {
        char raw[MYRTOS_DIRNAME_MAX], name[MYRTOS_DIRNAME_MAX + 2];
        uint32_t size = 0;
        int32_t attr = myrtos_fs_dir_at(dir, index, raw, &size);
        if (attr < 0) break;
        // The kernel hands back what FAT stores and a guest needs a name it can
        // pass straight back to open(). The rule is in common/myrtos_abi.h,
        // because ls, readdir and this must all expand it the same way; it was
        // copied here once and that was one copy too many.
        myrtos_pretty_name(raw, name);

        uint32_t namlen = 0;
        while (name[namlen]) namlen++;
        if (used + WASI_DIRENT_SIZE > buf_len) break;

        uint8_t *e = buf + used;
        for (uint32_t i = 0; i < WASI_DIRENT_SIZE; i++) e[i] = 0;
        wasi_put64(e, (uint64_t)(index + 1));                 // the next cookie
        wasi_put64(e + 8, wasi_inode(dir, name));
        wasi_put32(e + 16, namlen);
        e[20] = (attr & MYRTOS_ATTR_DIRECTORY) ? 3 : 4;
        used += WASI_DIRENT_SIZE;

        // A name that does not fit is still counted: the caller sees a short
        // buffer, comes back with a bigger one, and gets the whole entry then.
        uint32_t room = (buf_len > used) ? buf_len - used : 0;
        uint32_t take = (namlen < room) ? namlen : room;
        for (uint32_t i = 0; i < take; i++) buf[used + i] = (uint8_t)name[i];
        used += take;
        if (take < namlen) break;

        index++;
    }

    wasi_put32((uint8_t *)bufused, used);
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
    r = m3_LinkRawFunction(module, ns, "path_filestat_get",    "i(ii*i*)",   &wasi_path_filestat_get);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "path_unlink_file",     "i(i*i)",     &wasi_path_unlink_file);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_fdstat_set_flags",  "i(ii)",      &wasi_fd_fdstat_set_flags);
    if (r && r != m3Err_functionLookupFailed) return r;
    r = m3_LinkRawFunction(module, ns, "fd_readdir",           "i(i*iI*)",   &wasi_fd_readdir);
    if (r && r != m3Err_functionLookupFailed) return r;

    return m3Err_none;
}
