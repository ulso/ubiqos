// The system call layer. In OS-9 everything went through SWI2; here it is
// ecall, and the trap vector in scheduler.S is the way in.

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"
#include "video.h"
#include "chargen.h"

int32_t ubiqos_net_pid(uint32_t stack);

int32_t ubiqos_console_trace_at(uint32_t offset);
#include "trap.h"
#include "io.h"
#include "fat32.h"
#include "moddir.h"
#include "tlsf.h"
#include "crashlog.h"
#include "keystore.h"
#include "hardware/structs/rosc.h"
#include "hardware/structs/trng.h"
#include "hardware/resets.h"
#include "pico/time.h"
#include "critical.h"
#include "config.h"

void ubiqos_print(const char *s);
void ubiqos_putc(char c);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);
int32_t ubiqos_current_pid(void);
uint32_t ubiqos_process_count(void);
int32_t ubiqos_process_create(const ubiqos_module_header_t *m, const char *args);
uint32_t ubiqos_process_get_args(char *buf, uint32_t len);
void ubiqos_block_on_read(int32_t path);
bool    ubiqos_msg_send(int32_t dest, const ubiqos_msg_t *m);
int32_t ubiqos_msg_receive(ubiqos_msg_t *out);
int32_t ubiqos_msg_receive_tmo(ubiqos_msg_t *out, uint32_t ms);
int32_t ubiqos_msg_reply(int32_t status);
int32_t ubiqos_msg_reply_to(int32_t pid, int32_t status);
int32_t ubiqos_find_pid(const char *name);
int32_t ubiqos_console_select_font(int32_t index, ubiqos_confont_t *out, bool look_only);
int32_t ubiqos_process_kill(int32_t pid);
const char *ubiqos_cwd_get(void);
int32_t ubiqos_fs_server_pid(void);
int32_t ubiqos_wifi_server_pid(void);

// Hand a filesystem call to the server and block until it answers. The request
// is not copied: it points into the calling process's own memory, which cannot
// change because the process is stopped in send until the reply, and the
// reply's status lands in this frame's a0 as the call's return value.
//
// The work used to happen right here, in the trap, with interrupts off for as
// long as the card took. Zeroing a cluster is milliseconds, and the USB task is
// a process that cannot run while a trap is in progress -- so the keyboard was
// lost every time a directory was made.
static bool server_request(int32_t srv, uint32_t type, void *data) {
    if (srv < 0) return false;
    ubiqos_msg_t m;
    m.type = type;
    m.len  = 0;
    m.data = data;
    return ubiqos_msg_send(srv, &m);
}

// A descriptor read or write travels to the filesystem server as a message, and
// the message points at this rather than at anything on the trap stack -- which
// is gone the moment we switch away. One entry per process is enough because a
// process can only be blocked in one send at a time, which is the same argument
// that lets a sender's buffer be passed by pointer at all.
static ubiqos_fs_fdio_t fdio_req[UBIQOS_MAX_PROCESSES];

// And the same for a socket call, for the same reason and one that took a
// while to see: the message carries a POINTER, and a request left on the trap
// stack is only safe while the server reads it before the next trap reuses
// that stack. The NINA server is a process of its own and does; the lwIP
// server is the USB task, which looks once a millisecond, and in that window
// another trap overwrites the request -- which read as a socket operation of
// 2290649225 and varied from run to run depending on what else was happening.
static ubiqos_wifi_sock_t sock_req[UBIQOS_MAX_PROCESSES];
static ubiqos_fs_open_t open_req[UBIQOS_MAX_PROCESSES];
static ubiqos_fs_seek_t seek_req[UBIQOS_MAX_PROCESSES];
static ubiqos_fs_exec_t exec_req[UBIQOS_MAX_PROCESSES];

static bool fs_request(uint32_t type, void *data) {
    return server_request(ubiqos_fs_server_pid(), type, data);
}
void    ubiqos_cwd_inherit(int32_t parent, int32_t child);
bool    ubiqos_cwd_set(const char *abs);

void ubiqos_block_on_write(int32_t path);
bool ubiqos_block_on_child(int32_t pid);
void ubiqos_wake_readers(void);
void ubiqos_sleep_begin(uint32_t ticks);
uint32_t ubiqos_set_priority(uint32_t prio);
int32_t ubiqos_process_info(uint32_t slot, ubiqos_psinfo_t *out);
void ubiqos_reboot_bootsel(void);
void ubiqos_reboot_machine(void);
int32_t ubiqos_pulse_send(int32_t dest, uint32_t type, uint32_t value);
int32_t ubiqos_arm_read(int32_t path, uint32_t type);
int32_t ubiqos_disarm_reads(void);
uint32_t ubiqos_psram_bytes(void);
void *ubiqos_mem_alloc(uint32_t size);
void *ubiqos_mem_alloc_bulk(uint32_t size);
extern tlsf_pool_t ubiqos_bulk_pool;
void *ubiqos_process_data_area(uint32_t *size_out);
int32_t ubiqos_mem_free(void *ptr);
void *ubiqos_mem_realloc(void *ptr, uint32_t size);
void ubiqos_sleep_tick(void);
extern tlsf_pool_t ubiqos_mem_pool;

// The system call numbers come from common/ubiqos_abi.h, shared with modules.


volatile uint32_t ubiqos_ticks = 0;
volatile uint32_t ubiqos_trap_count = 0;
volatile uint32_t ubiqos_last_cause = 0;
volatile uint32_t ubiqos_last_pc = 0;

// The return value is the stack pointer to resume. Same in as out means we
// continue in the same process; a different one is a context switch.
// Raw samples from the RP2350's TRNG, for keys. Taken the way pico_rand takes
// them on this chip: the decorrelators bypassed, one ring-oscillator sample
// into the 192-bit EHR per clock, read out six words at a time. They are NOT
// uniform bits and are not handed out as if they were -- the caller hashes
// them, and the TLS code folds eight raw bytes into each byte it counts.
//
// Sixty-four bytes a call at most, because this runs in a trap with
// interrupts off: that is three EHR fills, microseconds. A TRNG that never
// finishes a fill gives back what it had rather than holding the trap.
static int32_t trng_raw(uint8_t *out, uint32_t want)
{
    static bool up;
    if (!up) {
        unreset_block_num_wait_blocking(RESET_TRNG);
        up = true;
    }
    if (want > 64) want = 64;
    uint32_t n = 0;
    while (n < want) {
        trng_hw->sample_cnt1 = 0;
        trng_hw->trng_debug_control = -1u;       // raw: no decorrelator, no checks
        trng_hw->rnd_source_enable = -1u;
        trng_hw->rng_icr = -1u;
        uint32_t spins = 0;
        while (trng_hw->trng_busy && ++spins < 100000u) {}
        if (trng_hw->trng_busy) break;
        for (uint32_t w = 0; w < 6 && n < want; w++) {
            const uint32_t v = trng_hw->ehr_data[w];
            for (int b = 0; b < 4 && n < want; b++) out[n++] = (uint8_t)(v >> (8 * b));
        }
    }
    return (int32_t)n;
}

uint32_t ubiqos_trap_handler(ubiqos_frame_t *frame) {
    uint32_t sp = (uint32_t)(uintptr_t)frame;

    ubiqos_trap_count++;
    ubiqos_last_cause = frame->cause;
    ubiqos_last_pc = frame->pc;

    if (UBIQOS_TRAP_IS_INTERRUPT(frame)) {
        if (UBIQOS_TRAP_IS_TIMER(frame)) {
            ubiqos_ticks++;
            // A blocked reader is woken here rather than by the driver: TinyUSB
            // delivers into its own buffers, and asking once per tick is both
            // simpler and enough at keyboard speed.
            ubiqos_wake_readers();
            ubiqos_sleep_tick();
            { extern void ubiqos_intr_tick(void); ubiqos_intr_tick(); }
// The interrupt stays pending until mtimecmp moves forward. Without
// this it recurs immediately and the machine does nothing else.
            ubiqos_timer_rearm();
// Time slicing: on every tick the next runnable process takes over.
            return ubiqos_switch(sp);
        }
        return sp;
    }

    if (UBIQOS_TRAP_IS_SYSCALL(frame)) {
// The pc points at the call instruction on one machine and past it on the
// other, so stepping over it is the machine's business. Without it
// mret returns to the same instruction and the machine loops.
        UBIQOS_TRAP_SKIP(frame);

        switch (frame->a7) {
        case SYS_NULL:
            frame->a0 = 0;
            break;
        case SYS_IO_PUTC:
            ubiqos_putc((char)frame->a0);
            frame->a0 = 0;
            break;
        case SYS_OPEN: {
            const char *name = (const char*)(uintptr_t)frame->a0;
            // A device is reached at /dev/name and nowhere else. Asking the
            // device table about bare names as well seemed harmless and was
            // not: a name that matched a device could never be a file, so
            // `echo hej > null` in any directory wrote to the null device and
            // created nothing. Six names were unusable that way, which is the
            // bug DOS had with CON for twenty years.
            //
            // A device is opened here, where it costs nothing. A file belongs to
            // the server, because walking a directory is long work and long work
            // in a trap is the mistake this system has already made four times.
            const char *devname = 0;
            if (name && name[0] == '/' && name[1] == 'd' && name[2] == 'e'
                     && name[3] == 'v' && name[4] == '/')
                devname = name + 5;
            if (devname) {
                frame->a0 = (uint32_t)ubiqos_io_open(devname, ubiqos_current_pid());
                break;
            }
            ubiqos_fs_open_t *o = &open_req[ubiqos_current_pid()];
            o->name = name;
            o->flags = frame->a1;
            if (!fs_request(UBIQOS_MSG_FS_OPEN, o)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_PIPE: {
            int32_t fds[2];
            if (ubiqos_io_pipe(fds, ubiqos_current_pid()) < 0) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            int32_t *out = (int32_t*)(uintptr_t)frame->a0;
            out[0] = fds[0];
            out[1] = fds[1];
            frame->a0 = 0;
            break;
        }
        case SYS_DUP:
            frame->a0 = (uint32_t)ubiqos_io_dup((int32_t)frame->a0, (int32_t)frame->a1,
                                                ubiqos_current_pid());
            break;
        case SYS_SEEK: {
            // From the end is the one that cannot be answered here: it needs
            // the file's length, and asking the filesystem for it is exactly
            // the long work a trap may not do. So it goes to the server the way
            // open does, and the process waits.
            if (frame->a2 == UBIQOS_SEEK_END) {
                ubiqos_fs_seek_t *k = &seek_req[ubiqos_current_pid()];
                k->fd = (int32_t)frame->a0;
                k->offset = (int32_t)frame->a1;
                if (!fs_request(UBIQOS_MSG_FS_SEEK, k)) {
                    frame->a0 = (uint32_t)-1;
                    break;
                }
                return ubiqos_switch(sp);
            }
            frame->a0 = (uint32_t)ubiqos_io_file_seek((int32_t)frame->a0,
                                                      ubiqos_current_pid(),
                                                      (int32_t)frame->a1, frame->a2);
            break;
        }
        case SYS_WRITE: {
            int32_t wpath = (int32_t)frame->a0;
            if (ubiqos_io_is_file(wpath, ubiqos_current_pid())) {
                ubiqos_fs_fdio_t *q = &fdio_req[ubiqos_current_pid()];
                q->fd = wpath;
                q->buf = (uint8_t*)(uintptr_t)frame->a1;
                q->len = frame->a2;
                q->write = 1;
                if (!fs_request(UBIQOS_MSG_FS_FDIO, q)) { frame->a0 = (uint32_t)-1; break; }
                return ubiqos_switch(sp);
            }
            int32_t wn = ubiqos_io_write(wpath, (const uint8_t*)(uintptr_t)frame->a1,
                                         frame->a2, ubiqos_current_pid());
            if (wn == 0 && frame->a2 && ubiqos_current_pid() != 0) {
                // No room. Same shape as a blocking read: step back onto the
                // ecall and wait, so the call is simply made again with its
                // arguments intact once the device can take something.
                UBIQOS_TRAP_REDO(frame);
                ubiqos_block_on_write(wpath);
                return ubiqos_switch(sp);
            }
            frame->a0 = (uint32_t)wn;
            break;
        }
        case SYS_SEND: {
            // No rewind here, unlike a blocking read: the call is not made
            // again. The reply writes its status straight into this frame's a0 and
            // the process resumes as though send had returned normally.
            if (!ubiqos_msg_send((int32_t)frame->a0,
                                 (const ubiqos_msg_t*)(uintptr_t)frame->a1)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_RECEIVE: {
            int32_t from = ubiqos_msg_receive((ubiqos_msg_t*)(uintptr_t)frame->a0);
            if (from == -1) return ubiqos_switch(sp);   // nothing yet; wait
            frame->a0 = (uint32_t)from;                 // -2 = reply first
            break;
        }
        case SYS_RECEIVETMO: {
            // -1 alone means the caller was put to sleep. A timeout is a value
            // like any other and returns through the frame, which is why it is
            // not -1: the two would be indistinguishable here.
            int32_t from = ubiqos_msg_receive_tmo((ubiqos_msg_t*)(uintptr_t)frame->a0,
                                                  frame->a1);
            if (from == -1) return ubiqos_switch(sp);
            frame->a0 = (uint32_t)from;
            break;
        }
        case SYS_REPLY:
            frame->a0 = (uint32_t)ubiqos_msg_reply((int32_t)frame->a0);
            break;
        case SYS_REPLYTO:
            frame->a0 = (uint32_t)ubiqos_msg_reply_to((int32_t)frame->a0,
                                                      (int32_t)frame->a1);
            break;
        case SYS_PIDOF:
            frame->a0 = (uint32_t)ubiqos_find_pid((const char*)(uintptr_t)frame->a0);
            break;
        case SYS_READ: {
            int32_t path = (int32_t)frame->a0;
            // A file at its end returns zero and does not block. Only a device
            // can have "nothing yet"; a file has nothing more.
            if (ubiqos_io_is_file(path, ubiqos_current_pid())) {
                ubiqos_fs_fdio_t *q = &fdio_req[ubiqos_current_pid()];
                q->fd = path;
                q->buf = (uint8_t*)(uintptr_t)frame->a1;
                q->len = frame->a2;
                q->write = 0;
                if (!fs_request(UBIQOS_MSG_FS_FDIO, q)) { frame->a0 = (uint32_t)-1; break; }
                return ubiqos_switch(sp);
            }
            int32_t n = ubiqos_io_read(path, (uint8_t*)(uintptr_t)frame->a1,
                                       frame->a2, ubiqos_current_pid());
            // Zero from a device means "not yet" and is worth waiting for.
            // Zero from a pipe whose writers have gone means "never", and a
            // process that waited for it would wait for ever.
            if (n == 0 && ubiqos_io_at_eof(path, ubiqos_current_pid())) {
                frame->a0 = 0;
                break;
            }
            if (n == 0 && ubiqos_current_pid() != 0) {
                // Nothing there. Step back onto the call and block: when
                // the process runs again it re-executes the call with its
                // arguments still in place, so nothing has to be remembered
                // about a half-finished read.
                UBIQOS_TRAP_REDO(frame);
                ubiqos_block_on_read(path);
                return ubiqos_switch(sp);
            }
            frame->a0 = (uint32_t)n;
            break;
        }
        case SYS_EXEC: {
// OS-9's F$Link and F$Fork in one: look the module up, bump
// its link count, and make a process of it. The code is shared --
// only the data area is new.
            const char *want = (const char*)(uintptr_t)frame->a0;
            const char *stored = ubiqos_moddir_match(want);
            if (!stored) { frame->a0 = (uint32_t)-1; break; }
            const ubiqos_module_header_t *m = ubiqos_moddir_link(stored);
            if (!m) { frame->a0 = (uint32_t)-1; break; }

// The creation itself goes to the filesystem server, because it is not the
// small operation it looks like: a single-instance module is copied to the
// address it was linked for, and for the interpreter that is a third of a
// megabyte from flash into PSRAM. Here that would run with interrupts off for
// tens of milliseconds and take the USB bus down with it -- a tight loop in a
// trap is a blackout whether or not it says so. Over there it runs in a thread,
// with everything else still able to run.
//
// The arguments are passed at creation: they are copied into the new process's
// own memory before the frame is built, so a0 can point past them. Passing the
// pointer raw is safe for the usual reason -- the send blocks, so this process
// stands still while the server reads it.
            ubiqos_fs_exec_t *e = &exec_req[ubiqos_current_pid()];
            e->module = m;
            e->args   = (const char*)(uintptr_t)frame->a1;
            if (fs_request(UBIQOS_MSG_FS_EXEC, e)) return ubiqos_switch(sp);

// Before the server exists -- the boot path starts its first processes this
// way -- there is nobody to ask, and nothing yet running that a blackout could
// hurt.
            int32_t pid = ubiqos_process_create(m, e->args);
            if (pid >= 0) {
                  ubiqos_io_inherit(ubiqos_current_pid(), pid);
                  ubiqos_cwd_inherit(ubiqos_current_pid(), pid);
              }
            frame->a0 = (uint32_t)pid;
            break;
        }
        case SYS_ARGS:
            frame->a0 = ubiqos_process_get_args((char*)(uintptr_t)frame->a0, frame->a1);
            break;
        // Reading a module off the card is filesystem work, so it goes where
        // all filesystem work goes. The caller blocks in send until the server
        // has it, and the reply's status is the call's result.
        case SYS_DISARM:
            frame->a0 = (uint32_t)ubiqos_disarm_reads();
            break;
        case SYS_ARM:
            frame->a0 = (uint32_t)ubiqos_arm_read((int32_t)frame->a0, frame->a1);
            break;
        case SYS_PULSE:
            frame->a0 = (uint32_t)ubiqos_pulse_send((int32_t)frame->a0,
                                                    frame->a1, frame->a2);
            break;
        case SYS_USBDISK:
            if (!fs_request(UBIQOS_MSG_FS_USBDISK, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_KEYS: {
            // Reading is cheap and answered here. Writing erases a sector, so
            // it goes to the filesystem server like every other long job --
            // the caller blocks in send, and the trap is over in a moment.
            ubiqos_keyreq_t *r = (ubiqos_keyreq_t*)(uintptr_t)frame->a1;
            if (!r) { frame->a0 = (uint32_t)-1; break; }
            switch (frame->a0) {
            case UBIQOS_KEY_OP_COUNT:
                frame->a0 = ubiqos_keys_count();
                break;
            case UBIQOS_KEY_OP_NTH:
                frame->a0 = ubiqos_keys_nth(r->index, r->name, &r->len) ? 0u : (uint32_t)-1;
                break;
            case UBIQOS_KEY_OP_PRINT:
                frame->a0 = ubiqos_keys_fingerprint(r->name, &r->fingerprint) ? 0u : (uint32_t)-1;
                break;
            case UBIQOS_KEY_OP_SET:
                if (!fs_request(UBIQOS_MSG_FS_KEYSET, r)) { frame->a0 = (uint32_t)-1; break; }
                return ubiqos_switch(sp);
            default:
                frame->a0 = (uint32_t)-1;
                break;
            }
            break;
        }
        case SYS_DATALINK: {
            // F$Link without the fork, for a data module: its bytes, and the
            // link that keeps them where they are until the caller lets go.
            const char *stored = ubiqos_moddir_match((const char*)(uintptr_t)frame->a0);
            const ubiqos_module_header_t *m = stored ? ubiqos_moddir_link(stored) : 0;
            if (m && (m->type_lang >> 8) != UBIQOS_TYPE_DATA) {
                ubiqos_moddir_unlink(m);
                m = 0;
            }
            if (!m) { frame->a0 = 0; break; }
            uint32_t *size = (uint32_t*)(uintptr_t)frame->a1;
            if (size) *size = m->module_size - (uint32_t)sizeof(ubiqos_module_header_t);
            frame->a0 = (uint32_t)(uintptr_t)m + (uint32_t)sizeof(ubiqos_module_header_t);
            break;
        }
        case SYS_DATAUNLINK:
            if (frame->a0 > sizeof(ubiqos_module_header_t))
                ubiqos_moddir_unlink((const ubiqos_module_header_t*)(uintptr_t)
                                     (frame->a0 - sizeof(ubiqos_module_header_t)));
            frame->a0 = 0;
            break;
        case SYS_LOADMOD:
            if (!fs_request(UBIQOS_MSG_FS_LOADMOD, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_FSDIR:
            if (!fs_request(UBIQOS_MSG_FS_DIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_CHDIR:
            if (!fs_request(UBIQOS_MSG_FS_CHDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_GETCWD: {
            char *out = (char*)(uintptr_t)frame->a0;
            const char *cwd = ubiqos_cwd_get();
            uint32_t i = 0;
            while (cwd[i] && i + 1 < frame->a1) { out[i] = cwd[i]; i++; }
            out[i] = 0;
            frame->a0 = i;
            break;
        }
        case SYS_MKDIR:
            if (!fs_request(UBIQOS_MSG_FS_MKDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_RMDIR:
            if (!fs_request(UBIQOS_MSG_FS_RMDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_WIFIADDR: {
            ubiqos_wifi_req_t req;
            req.index = 0;
            req.buf   = (char*)(uintptr_t)frame->a0;
            req.len   = frame->a1;
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_ADDR, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_WIFIJOIN: {
            // Through the service, like the others: joining waits seconds and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            ubiqos_wifi_req_t req;
            req.index = 0;
            // No argument means the network in /sd/config.txt. The bytes are
            // the kernel's and stay the kernel's: the caller asked to join,
            // not to be told the password.
            req.buf   = frame->a0 ? (char*)(uintptr_t)frame->a0
                                  : (char*)ubiqos_config_credentials();
            req.len   = 0;
            if (!req.buf) { frame->a0 = (uint32_t)-1; break; }
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_JOIN, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_WIFIRESET: {
            // Not through the service. A reset is three quarters of a second of
            // waiting, which is why it does NOT belong in a trap -- so it goes
            // to the wifi thread like everything else that talks to that chip.
            ubiqos_wifi_sock_t req;
            req.op = 0; req.arg = 0; req.buf = 0; req.len = 0;
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_RESET, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_WIFISOCK: {
            // Every socket call goes through the service for the same reason
            // the others do: it talks to the chip over SPI with handshakes and
            // waits, and waiting inside a trap stops the machine. The request
            // sits on this stack, which is safe because send blocks until the
            // answer -- the same argument as the three above.
            // Which stack, and therefore which server. LISTEN_ON names it in
            // the argument because there is no socket yet; everything else
            // reads it out of the socket number, which carries it.
            uint32_t stack;
            uint32_t arg = frame->a1;
            if (frame->a0 == UBIQOS_SOCK_LISTEN_ON
                || frame->a0 == UBIQOS_SOCK_CONNECT) {
                // Both name the stack in the argument for the same reason:
                // there is no socket yet to carry it.
                stack = (arg >> 16) & 0xffu;
                arg   = arg & 0xffffu;              // the port
            } else if (frame->a0 == UBIQOS_SOCK_LISTEN) {
                stack = UBIQOS_NET_NINA;
            } else {
                stack = UBIQOS_SOCK_STACK(arg);
                arg   = UBIQOS_SOCK_INDEX(arg);     // the stack never sees its own byte
            }

            int32_t npid = ubiqos_net_pid(stack);
            if (npid < 0) { frame->a0 = (uint32_t)-1; break; }

            ubiqos_wifi_sock_t *rq = &sock_req[ubiqos_current_pid()];
            // Both listens are one operation to the server; the choice of
            // server is what the two forms differ by, and that is settled.
            rq->op  = (frame->a0 == UBIQOS_SOCK_LISTEN_ON) ? UBIQOS_SOCK_LISTEN : frame->a0;
            rq->arg = arg;
            rq->buf = 0;
            rq->len = 0;
            if (frame->a2) {
                // The buffer and its length arrive together, because a syscall
                // has three arguments and this wants four.
                const ubiqos_sockbuf_t *b = (const ubiqos_sockbuf_t*)(uintptr_t)frame->a2;
                rq->buf = b->buf;
                rq->len = b->len;
            }
            if (!server_request(npid, UBIQOS_MSG_WIFI_SOCK, rq)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            // Nothing is done to the answer. A stack returns socket numbers
            // already carrying its own byte -- NINA's are 0 to 9 and its byte
            // is zero, so they are unchanged, and a second stack tags its own.
            // Re-tagging here would mean reaching into the reply on its way
            // back to the caller, and there is no reason to: the stack knows
            // which stack it is.
            return ubiqos_switch(sp);
        }
        case SYS_WIFISCAN: {
            // Through the service rather than here: a scan waits seconds, and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            ubiqos_wifi_req_t req;
            req.index = (int32_t)frame->a0;
            req.buf   = (char*)(uintptr_t)frame->a1;
            req.len   = frame->a2;
            if (UBIQOS_MSG_WIFI_SCAN == UBIQOS_MSG_WIFI_VER) {
                req.buf = (char*)(uintptr_t)frame->a0;
                req.len = frame->a1;
            }
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_SCAN, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_WIFIVER: {
            // Through the service rather than here: a scan waits seconds, and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            ubiqos_wifi_req_t req;
            req.index = (int32_t)frame->a0;
            req.buf   = (char*)(uintptr_t)frame->a1;
            req.len   = frame->a2;
            if (UBIQOS_MSG_WIFI_VER == UBIQOS_MSG_WIFI_VER) {
                req.buf = (char*)(uintptr_t)frame->a0;
                req.len = frame->a1;
            }
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_VER, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_WIFISTATS: {
            // The same shape as SYS_WIFIVER and for the same reason: the
            // counters live in the library, which runs in the wifi thread, and
            // a trap is not where one waits for anything.
            ubiqos_wifi_req_t req;
            req.index = 0;
            req.buf   = (char*)(uintptr_t)frame->a0;
            req.len   = 32;
            if (!server_request(ubiqos_wifi_server_pid(), UBIQOS_MSG_WIFI_STATS, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        }
        case SYS_FSSTAT:
            if (!fs_request(UBIQOS_MSG_FS_STAT, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_MOUNT:
            if (!fs_request(UBIQOS_MSG_FS_MOUNT, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_FOREGRND:
            frame->a0 = (uint32_t)ubiqos_io_set_foreground(
                (int32_t)frame->a0, (int32_t)frame->a1, ubiqos_current_pid());
            break;
        case SYS_KILL: {
            int32_t victim = (int32_t)frame->a0;
            // Killing yourself is exiting, and exiting never returns.
            if (victim == ubiqos_current_pid()) {
                ubiqos_process_exit();
                return ubiqos_switch(sp);
            }
            // The same courtesy Ctrl-C got: a process that asked to hear about
            // being ended is told, and has half a second to do it itself. That
            // is the only way a background process can ever stop cleanly -- it
            // is nobody's foreground, so the key cannot reach it. a1 set is the
            // one that does not ask, which is what -9 has always meant.
            extern bool ubiqos_intr_request(int32_t pid);
            if (!frame->a1 && ubiqos_intr_request(victim)) { frame->a0 = 0; break; }
            frame->a0 = (uint32_t)ubiqos_process_kill(victim);
            break;
        }
        case SYS_READABLE:
            frame->a0 = (uint32_t)ubiqos_io_readable_count((int32_t)frame->a0,
                                                           ubiqos_current_pid());
            break;
        case SYS_CONFONT:
            // Cheap enough to serve here: it records which font is wanted and
            // fills in a struct of six bytes. The console's own thread does the
            // work, on its own time, once it has drawn what was already queued.
            frame->a0 = (uint32_t)ubiqos_console_select_font(
                (int32_t)frame->a0, (ubiqos_confont_t*)(uintptr_t)frame->a1,
                frame->a2 != 0);
            break;
        case SYS_FSREAD:
            if (!fs_request(UBIQOS_MSG_FS_READ, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_FSWRITE:
            if (!fs_request(UBIQOS_MSG_FS_WRITE, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_FSREMOVE:
            if (!fs_request(UBIQOS_MSG_FS_REMOVE, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_FSRENAME:
            // The pair travels by pointer into the caller's own memory, which
            // is stable because send blocks the caller until the server has
            // answered -- the same argument as every other request here.
            if (!fs_request(UBIQOS_MSG_FS_RENAME, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return ubiqos_switch(sp);
        case SYS_CRITHOLD: {
            // Deliberately long, which is the opposite of what every other
            // critical section here tries to be. It is the only way to ask
            // whether a handler above the threshold really is let through:
            // real sections are under three microseconds and both mechanisms
            // look identical against them.
            uint32_t us = frame->a0;
            if (us > 5000u) us = 5000u;      // a test, not a way to hang the machine
            uint32_t st = ubiqos_critical_enter();
            busy_wait_us(us);
            ubiqos_critical_exit(st);
            frame->a0 = 0;
            break;
        }
        case SYS_CONFIG: {
            // The password is the one setting with no read. Asking for it
            // answers whether there is one, which is all a caller needs to know
            // to decide whether to prompt -- and is the difference between a
            // secret kept in the kernel and a secret with a system call in
            // front of it.
            if (frame->a0 == UBIQOS_CFG_PASSWORD) {
                frame->a0 = ubiqos_config_has_password() ? 1u : 0u;
                break;
            }
            const char *v = frame->a0 == UBIQOS_CFG_HOSTNAME
                          ? ubiqos_config_hostname()
                          : frame->a0 == UBIQOS_CFG_SSID ? ubiqos_config_ssid() : 0;
            if (!v) { frame->a0 = (uint32_t)-1; break; }
            char *out = (char *)(uintptr_t)frame->a1;
            uint32_t cap = frame->a2, n = 0;
            if (!out || !cap) { frame->a0 = (uint32_t)-1; break; }
            while (v[n] && n < cap - 1) { out[n] = v[n]; n++; }
            out[n] = 0;
            frame->a0 = n;
            break;
        }
        case SYS_NETDEV: {
#if UBIQOS_LWIP
            extern uint32_t ubiqos_lwip_in, ubiqos_lwip_out, ubiqos_lwip_dropped;
            extern uint32_t ubiqos_lwip_addr(void);
            extern bool ubiqos_lwip_started(void);
            extern uint8_t tud_network_mac_address[6];
            uint32_t *o = (uint32_t *)(uintptr_t)frame->a0;
            o[0] = ubiqos_lwip_started() ? 1u : 0u;
            o[1] = ubiqos_lwip_in;
            o[2] = ubiqos_lwip_addr();
            o[3] = ubiqos_lwip_out;
            o[4] = ubiqos_lwip_dropped;
            { extern void ubiqos_lwip_stats(uint32_t *); ubiqos_lwip_stats(o + 6); }
            o[5] = ((uint32_t)tud_network_mac_address[2] << 24) |
                   ((uint32_t)tud_network_mac_address[3] << 16) |
                   ((uint32_t)tud_network_mac_address[4] << 8) |
                    (uint32_t)tud_network_mac_address[5];
            frame->a0 = 0;
#else
            frame->a0 = (uint32_t)-1;   // this build has no network stack
#endif
            break;
        }
        case SYS_VIDSTAT: {
            // a1 says what is wanted, a2 is its index. Kind 3 is not about the
            // display at all and works in either build, which is the point of
            // it: the same trace can be taken from a framebuffer kernel and
            // compared.
            uint8_t *out = (uint8_t *)(uintptr_t)frame->a0;
            if (frame->a1 == 3) {
                uint32_t i = 0;
                for (; i < 64; i++) {
                    int32_t c = ubiqos_console_trace_at(frame->a2 + i);
                    if (c < 0) break;
                    out[i] = (uint8_t)c;
                }
                frame->a0 = i;
                break;
            }
            // Kind 6: one whole scanline as the display shows it, RGB565, the
            // width in halfwords into a0. Only the RGB panel can say, and the
            // question is answered here, before the kinds below: on any other
            // display an unknown kind falls through to the statistics, which
            // would write sixteen words into a buffer meant for a line.
            if (frame->a1 == 6) {
#if UBIQOS_VIDEO_RGB
                uint32_t ubiqos_video_capture_line(uint32_t, uint16_t *);
                frame->a0 = ubiqos_video_capture_line(frame->a2, (uint16_t *)(uintptr_t)frame->a0);
#else
                frame->a0 = (uint32_t)-1;
#endif
                break;
            }
#if UBIQOS_VIDEO_RGB
            // Kind 4 gives the panel to an application: a list of things to
            // draw, which the video interrupt rasterises one band at a time.
            // Data and not a callback, because a module's code is in flash and
            // nothing reached from an interrupt may be. A count of zero hands
            // the screen back to the character generator.
            if (frame->a1 == 5) {
                int32_t ubiqos_video_backlight(uint32_t);
                frame->a0 = (uint32_t)ubiqos_video_backlight(frame->a2);
                break;
            }
            if (frame->a1 == 4) {
                int32_t ubiqos_video_set_scene(const ubiqos_draw_item_t *, uint32_t);
                frame->a0 = (uint32_t)ubiqos_video_set_scene(
                    (const ubiqos_draw_item_t *)(uintptr_t)frame->a0, frame->a2);
                break;
            }
#endif
#if UBIQOS_VIDEO_CHARGEN
            if (frame->a1 == 2)      ubiqos_chargen_peek_row(frame->a2, out, 80);
            else if (frame->a1 == 1) ubiqos_video_peek_line(frame->a2, out, 64);
            else                     ubiqos_video_stats_fill((uint32_t *)out);
            frame->a0 = 0;
#else
            frame->a0 = (uint32_t)-1;    // a framebuffer keeps up by existing
#endif
            break;
        }
        case SYS_GETSTAT:
        case SYS_SETSTAT: {
            const ubiqos_stat_t *a = (const ubiqos_stat_t *)(uintptr_t)frame->a2;
            if (!a) { frame->a0 = (uint32_t)-1; break; }
            // Files go nowhere yet. The filesystem's own questions -- how big,
            // when written -- are what SYS_FSSTAT already answers, and giving
            // a file a second way to be asked before anything wants one would
            // be inventing a shape rather than finding it.
            if (ubiqos_io_is_file((int32_t)frame->a0, ubiqos_current_pid())) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            int32_t r = (frame->a7 == SYS_GETSTAT)
                ? ubiqos_io_getstat((int32_t)frame->a0, frame->a1, a->data, a->len,
                                    ubiqos_current_pid())
                : ubiqos_io_setstat((int32_t)frame->a0, frame->a1, a->data, a->len,
                                    ubiqos_current_pid());
            frame->a0 = (uint32_t)r;
            break;
        }
        case SYS_CLOSE:
            frame->a0 = (uint32_t)ubiqos_io_close((int32_t)frame->a0, ubiqos_current_pid());
            break;
        case SYS_MODDIR: {
            const ubiqos_module_entry_t *e = ubiqos_moddir_entry(frame->a0);
            if (!e) { frame->a0 = (uint32_t)-1; break; }
            ubiqos_modinfo_t *out = (ubiqos_modinfo_t*)(uintptr_t)frame->a1;
            for (int i = 0; i < UBIQOS_NAME_LEN; i++) out->name[i] = e->name[i];
            out->links = e->links;
            out->revision = e->header->revision;
            out->size = e->header->module_size;
            out->type = (uint32_t)(e->header->type_lang >> 8);
            frame->a0 = 0;
            break;
        }
        case SYS_MEMINFO:
            if (frame->a0 == UBIQOS_MEM_BULK_FREE) {
                frame->a0 = ubiqos_bulk_pool
                          ? (uint32_t)ubiqos_tlsf_largest_free(ubiqos_bulk_pool) : 0;
                break;
            }
            if (frame->a0 == UBIQOS_MEM_BULK_SIZE) {
                frame->a0 = ubiqos_bulk_pool ? (uint32_t)ubiqos_psram_bytes() : 0;
                break;
            }
            if (frame->a0 == UBIQOS_MEM_ASSERTS)     { frame->a0 = ubiqos_asserts_seen; break; }
            if (frame->a0 == UBIQOS_MEM_ASSERT_LAST) { frame->a0 = ubiqos_assert_last;  break; }
            frame->a0 = (frame->a0 == UBIQOS_MEM_PROCESSES)
                ? ubiqos_process_count()
                : (uint32_t)ubiqos_tlsf_largest_free(ubiqos_mem_pool);
            break;
        case SYS_CATCHINTR: {
            extern int32_t ubiqos_intr_catch(uint32_t type);
            frame->a0 = (uint32_t)ubiqos_intr_catch(frame->a0);
            break;
        }
        case SYS_RANDOM: {
            // The ring oscillator's random bit, thirty-two of them to a word.
            //
            // The SDK has get_rand_32, and it was tried first: pico_rand brings
            // a board id, a hash of RAM and a 128-bit generator with it, and
            // cost four kilobytes of a C heap that has 4580 bytes spare. This
            // reads the same oscillator directly and costs nothing.
            //
            // Sampled fast the bit correlates with itself, so the microsecond
            // timer is mixed in -- and even so this is entropy for seeding a
            // hash or picking an identifier, not for a key. Nothing here should
            // pretend otherwise.
            uint8_t *out = (uint8_t*)(uintptr_t)frame->a0;
            uint32_t want = frame->a1;
            if (!out) { frame->a0 = 0; break; }
            if (frame->a2 & UBIQOS_RANDOM_TRNG) {
                frame->a0 = (uint32_t)trng_raw(out, want);
                break;
            }
            if (want > 256) want = 256;          // see the note in the header
            uint32_t n = 0;
            while (n < want) {
                uint32_t r = 0;
                for (int b = 0; b < 32; b++) r = (r << 1) | (rosc_hw->randombit & 1u);
                r ^= (uint32_t)time_us_64();
                for (int b = 0; b < 4 && n < want; b++) out[n++] = (uint8_t)(r >> (8 * b));
            }
            frame->a0 = n;
            break;
        }
        case SYS_USBINFO: {
            extern uint32_t ubiqos_usbhost_info(uint32_t what);
            frame->a0 = ubiqos_usbhost_info(frame->a0);
            break;
        }
        case SYS_DATAAREA:
            frame->a0 = (uint32_t)(uintptr_t)ubiqos_process_data_area(
                            (uint32_t*)(uintptr_t)frame->a0);
            break;
        case SYS_ALLOCBULK:
            frame->a0 = (uint32_t)(uintptr_t)ubiqos_mem_alloc_bulk(frame->a0);
            break;
        case SYS_ALLOC:
            frame->a0 = (uint32_t)(uintptr_t)ubiqos_mem_alloc(frame->a0);
            break;
        case SYS_FREE:
            frame->a0 = (uint32_t)ubiqos_mem_free((void*)(uintptr_t)frame->a0);
            break;
        case SYS_REALLOC:
            frame->a0 = (uint32_t)(uintptr_t)ubiqos_mem_realloc(
                            (void*)(uintptr_t)frame->a0, frame->a1);
            break;
        case SYS_BOOTSEL:
            ubiqos_reboot_bootsel();    // does not return
            break;
        case SYS_REBOOT:
            ubiqos_reboot_machine();    // does not return either
            break;
        case SYS_PSINFO:
            frame->a0 = (uint32_t)ubiqos_process_info(frame->a0,
                            (ubiqos_psinfo_t*)(uintptr_t)frame->a1);
            break;
        case SYS_TICKS:
            frame->a0 = (uint32_t)ubiqos_ticks;
            break;
        case SYS_SETPRIO:
            frame->a0 = ubiqos_set_priority(frame->a0);
            break;
        case SYS_SLEEP: {
            uint32_t ms = frame->a0;                // read before a0 is the result
            frame->a0 = 0;
            if (ubiqos_current_pid() == 0) break;   // the kernel does not sleep
            if (ms == 0) return ubiqos_switch(sp);  // zero is a yield
            ubiqos_sleep_begin(ms);
            return ubiqos_switch(sp);
        }
        case SYS_WAIT:
            if (ubiqos_block_on_child((int32_t)frame->a0)) {
                frame->a0 = 0;
                return ubiqos_switch(sp);
            }
            frame->a0 = 0;      // already gone; nothing to wait for
            break;
        case SYS_EXIT:
// The process is not to be resumed, so we switch away at once.
            ubiqos_process_exit();
            return ubiqos_switch(sp);
        default:
            // The number matters: without it the message says only that
            // something is wrong, which cost an hour when a stale kernel met a
            // module built against a newer one.
            ubiqos_print("*** UBIQOS: unknown system call ");
            ubiqos_print_u32(frame->a7);
            ubiqos_print(" ***\n");
            frame->a0 = (uint32_t)-1;
            break;
        }
        return sp;
    }

    // An ebreak is an assertion, not a fault, and stepping over it is what the
    // code that executed it expects.
    //
    // TinyUSB ends every failed TU_ASSERT in TU_BREAKPOINT. On ARM that macro
    // reads DHCSR and halts *only if a debugger is attached*; on RISC-V it is an
    // unconditional ebreak. So the same failed assertion returns false and lets
    // the stack carry on for most of TinyUSB's users, and stops this machine
    // dead. That is how the keyboard died: an assertion in cdch_xfer_cb, the
    // core parked on the ebreak, USB host gone, and nothing said anything. The
    // SD driver's __breakpoint() had already cost a boot for exactly this.
    //
    // Past the ebreak is `li a0, 0; ret` -- TU_ASSERT's own false. So step over
    // it. How far is the machine's business and lives in its trap.h: the width
    // of an ebreak on one, a fixed two bytes and a sticky bit to clear on the
    // other.
    //
    // The tally matters as much as the step. A stack that asserts on every poll
    // would otherwise look like a machine that works, so the first one is
    // printed and all of them are counted.
    if (UBIQOS_TRAP_IS_BREAKPOINT(frame)) {
        if (!ubiqos_asserts_seen) {
            ubiqos_crash_note(UBIQOS_CRASH_ASSERT, frame->pc, 0, 0);
            // Hex, because the only thing anyone does with this number is
            // look it up with addr2line. Printed in decimal it cost a round
            // trip to convert, the first time it ever fired in front of a user.
            ubiqos_print("\n*** UBIQOS: assertion at ");
            ubiqos_print_hex(frame->pc);
            ubiqos_print(", stepped over ***\n");
        }
        ubiqos_asserts_seen++;
        ubiqos_assert_last = frame->pc;
        UBIQOS_TRAP_STEP_BREAKPOINT(frame);
        return sp;
    }

    // Write it down before saying anything, because saying it goes through the
    // console -- and a fault this early is usually a fault on the way to having
    // one. What the probe reads must not depend on the screen ever working.
    ubiqos_crash_note(UBIQOS_CRASH_TRAP, frame->pc, frame->cause, UBIQOS_TRAP_FAULT(frame));

    ubiqos_print("\n*** UBIQOS TRAP: unhandled exception ***\n");
    ubiqos_print("  pc ");     ubiqos_print_u32(frame->pc);
    ubiqos_print("  cause ");  ubiqos_print_u32(frame->cause);
    ubiqos_print("  fault ");  ubiqos_print_u32(UBIQOS_TRAP_FAULT(frame));
    ubiqos_print("\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
