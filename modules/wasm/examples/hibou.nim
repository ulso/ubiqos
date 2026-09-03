# The sensor scanner in Nim, which reaches wasm the same way it reaches anything
# else: it writes C and then drives a C compiler. One command, no separate step:
#
#   SYSROOT=$(brew --prefix wasi-libc)/share/wasi-sysroot
#   nim c --cpu:wasm32 --os:any --mm:arc -d:useMalloc -d:release --opt:size \
#         --noMain --cc:clang \
#         --clang.exe:$(brew --prefix llvm)/bin/clang \
#         --clang.linkerexe:$(brew --prefix llvm)/bin/clang \
#         --passC:"--target=wasm32-wasip1 --sysroot=$SYSROOT -fno-builtin \
#                  -D_WASI_EMULATED_SIGNAL -ffunction-sections -fdata-sections" \
#         --passL:"--target=wasm32-wasip1 --sysroot=$SYSROOT \
#                  -lwasi-emulated-signal -Wl,--gc-sections -Wl,--strip-all" \
#         --out:hibou.wasm hibou.nim
#
# Four things are load-bearing there and each cost an attempt:
#
#   -d:useMalloc    --os:any has no memory manager, and without this Nim stops
#                   with "Port memory manager to your platform".
#   _WASI_EMULATED_SIGNAL   Nim's system module reaches for SIGINT and SIGSEGV.
#                   wasm has no signals; the stub library satisfies the names.
#   --noMain, and the entry proc exported as "main" rather than "_start" --
#                   wasi-libc's crt1 owns _start and calls main, and exporting
#                   _start here is a duplicate symbol.
#   --mm:arc        reference counting with no cycle collector, which is what
#                   makes the runtime small enough to be worth measuring.
#
# The WASI calls are imported by hand, the same way tiny.c does it, so the two
# can be compared fairly.
{.emit: """
#define WASI __attribute__((import_module("wasi_snapshot_preview1")))
typedef struct { const void *buf; unsigned len; } wiov;
WASI __attribute__((import_name("path_open")))
int nw_path_open(int, unsigned, const char*, unsigned, unsigned,
                 unsigned long long, unsigned long long, unsigned, int*);
WASI __attribute__((import_name("fd_read")))
int nw_fd_read(int, const wiov*, unsigned, unsigned*);
WASI __attribute__((import_name("fd_write")))
int nw_fd_write(int, const wiov*, unsigned, unsigned*);
WASI __attribute__((import_name("poll_oneoff")))
int nw_poll(const void*, void*, unsigned, unsigned*);
WASI __attribute__((import_name("proc_exit"))) void nw_exit(unsigned);
""".}

type WIov {.importc: "wiov", nodecl, bycopy.} = object
  buf: pointer
  len: uint32

proc pathOpen(d: cint; df: uint32; p: cstring; pl, of1: uint32;
              rb, ri: uint64; ff: uint32; fd: ptr cint): cint
  {.importc: "nw_path_open", nodecl.}
proc fdRead(fd: cint; v: ptr WIov; n: uint32; got: ptr uint32): cint
  {.importc: "nw_fd_read", nodecl.}
proc fdWrite(fd: cint; v: ptr WIov; n: uint32; put: ptr uint32): cint
  {.importc: "nw_fd_write", nodecl.}
proc pollOneoff(i: pointer; o: pointer; n: uint32; ev: ptr uint32): cint
  {.importc: "nw_poll", nodecl.}
proc procExit(c: uint32) {.importc: "nw_exit", nodecl.}

proc say(s: cstring; n: uint32) =
  var v = WIov(buf: cast[pointer](s), len: n)
  var g: uint32
  discard fdWrite(1, addr v, 1, addr g)

proc pauseMs(ms: uint64) =
  var sub: array[48, byte]
  var ev: array[32, byte]
  let ns = ms * 1_000_000'u64
  copyMem(addr sub[24], addr (var t = ns; t), 8)
  var n: uint32
  discard pollOneoff(addr sub[0], addr ev[0], 1, addr n)

proc nimMain(): cint {.exportc: "main".} =
  const dev = "dev/acm"
  var fd: cint = -1
  if pathOpen(3, 0, dev.cstring, uint32(dev.len), 0,
              0xFFFFFFFFFFFFFFFF'u64, 0xFFFFFFFFFFFFFFFF'u64, 0, addr fd) != 0 or fd < 0:
    say("nim: no dongle\n", 15)
    procExit(1)

  const setup = ["ATE0\r", "AT+CENTRAL\r", "AT+FINDSCANDATA=FF5B07\r"]
  for s in setup:
    var v = WIov(buf: cast[pointer](s.cstring), len: uint32(s.len))
    var g: uint32
    discard fdWrite(fd, addr v, 1, addr g)
    pauseMs(300)

  say("nim: scanning\n", 14)

  var buf: array[256, byte]
  while true:
    var v = WIov(buf: addr buf[0], len: 256)
    var got: uint32 = 0
    if fdRead(fd, addr v, 1, addr got) != 0 or got == 0: break
    var o = WIov(buf: addr buf[0], len: got)
    var p: uint32
    discard fdWrite(1, addr o, 1, addr p)
  procExit(0)
