// The same program with no libc at all: the WASI calls are imported by hand.
//
// 925 bytes, against 13813 for the same thing written against POSIX. The whole
// of the difference is wasi-libc, and almost all of THAT is one function of
// 6.9 kB: dlmalloc, pulled in because opening a file makes libc build its
// preopen table on the heap. Dropping stdio for plain write() saved only two
// kilobytes; dropping libc saved twelve.
//
// The cost is that this is not portable C any more -- it will not build for the
// host, and it cannot use a library that expects a libc. Worth it for something
// small that talks to a device; not worth it for anything that has to compute.
//
//   clang --target=wasm32 -Oz -fno-builtin -nostdlib \
//         -Wl,--no-entry -Wl,--export=_start -Wl,--strip-all tiny.c -o tiny.wasm
//
// -fno-builtin matters: without it clang recognises the hand-written length
// loop below and replaces it with a call to strlen, which is not there.
#define WASI __attribute__((import_module("wasi_snapshot_preview1")))
typedef unsigned int u32; typedef unsigned long long u64;
typedef struct { const void *buf; u32 len; } iov;

WASI __attribute__((import_name("path_open")))
int path_open(int dirfd, u32 flags, const char *p, u32 plen, u32 oflags,
              u64 rights, u64 inheriting, u32 fdflags, int *fd);
WASI __attribute__((import_name("fd_read")))
int fd_read(int fd, const iov *v, u32 n, u32 *got);
WASI __attribute__((import_name("fd_write")))
int fd_write(int fd, const iov *v, u32 n, u32 *put);
WASI __attribute__((import_name("poll_oneoff")))
int poll_oneoff(const void *in, void *out, u32 n, u32 *nev);
WASI __attribute__((import_name("proc_exit"))) void proc_exit(u32 c);

static u32 len(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { iov v = { s, len(s) }; u32 g; fd_write(1, &v, 1, &g); }

static void pause_ms(u64 ms) {
    unsigned char sub[48] = {0}, ev[32];
    u64 ns = ms * 1000000ull;
    __builtin_memcpy(sub + 24, &ns, 8);       // relative timeout, clock id 0, flags 0
    u32 n = 0;
    poll_oneoff(sub, ev, 1, &n);
}

void _start(void) {
    // Relative to the preopen, with no leading slash: the host puts the slash
    // back itself. Passing "/dev/acm" here opens something that is not the
    // dongle and every read of it returns nothing, which looks exactly like a
    // dongle that is not plugged in.
    static const char dev[] = "dev/acm";
    int fd = -1;
    // Rights: everything. The host does not check them.
    if (path_open(3, 0, dev, sizeof(dev) - 1, 0, ~0ull, ~0ull, 0, &fd) || fd < 0) {
        say("tiny: no dongle\n"); proc_exit(1);
    }
    static const char *setup[] = { "ATE0\r", "AT+CENTRAL\r", "AT+FINDSCANDATA=FF5B07\r" };
    for (int i = 0; i < 3; i++) {
        iov v = { setup[i], len(setup[i]) }; u32 g;
        fd_write(fd, &v, 1, &g);
        pause_ms(300);
    }
    say("tiny: scanning\n");
    for (;;) {
        static char buf[256];
        iov v = { buf, sizeof buf }; u32 got = 0;
        if (fd_read(fd, &v, 1, &got) || !got) break;
        iov o = { buf, got }; u32 p;
        fd_write(1, &o, 1, &p);
    }
    proc_exit(0);
}
