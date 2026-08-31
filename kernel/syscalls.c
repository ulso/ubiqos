// The system call layer. In OS-9 everything went through SWI2; here it is
// ecall, and the trap vector in scheduler.S is the way in.

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"
#include "trap.h"
#include "io.h"
#include "fat32.h"
#include "moddir.h"
#include "tlsf.h"
#include "crashlog.h"

void myrtos_print(const char *s);
void myrtos_putc(char c);
void myrtos_print_u32(uint32_t v);
int32_t myrtos_current_pid(void);
uint32_t myrtos_process_count(void);
int32_t myrtos_process_create(const myrtos_module_header_t *m, const char *args);
uint32_t myrtos_process_get_args(char *buf, uint32_t len);
void myrtos_block_on_read(int32_t path);
bool    myrtos_msg_send(int32_t dest, const myrtos_msg_t *m);
int32_t myrtos_msg_receive(myrtos_msg_t *out);
int32_t myrtos_msg_reply(int32_t status);
int32_t myrtos_msg_reply_to(int32_t pid, int32_t status);
int32_t myrtos_find_pid(const char *name);
int32_t myrtos_console_select_font(int32_t index, myrtos_confont_t *out, bool look_only);
int32_t myrtos_process_kill(int32_t pid);
const char *myrtos_cwd_get(void);
int32_t myrtos_fs_server_pid(void);
int32_t myrtos_wifi_server_pid(void);

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
    myrtos_msg_t m;
    m.type = type;
    m.len  = 0;
    m.data = data;
    return myrtos_msg_send(srv, &m);
}

static bool fs_request(uint32_t type, void *data) {
    return server_request(myrtos_fs_server_pid(), type, data);
}
void    myrtos_cwd_inherit(int32_t parent, int32_t child);
bool    myrtos_cwd_set(const char *abs);

void myrtos_block_on_write(int32_t path);
bool myrtos_block_on_child(int32_t pid);
void myrtos_wake_readers(void);
void myrtos_sleep_begin(uint32_t ticks);
uint32_t myrtos_set_priority(uint32_t prio);
int32_t myrtos_process_info(uint32_t slot, myrtos_psinfo_t *out);
void myrtos_reboot_bootsel(void);
uint32_t myrtos_psram_bytes(void);
void *myrtos_mem_alloc(uint32_t size);
void *myrtos_mem_alloc_bulk(uint32_t size);
extern tlsf_pool_t myrtos_bulk_pool;
void *myrtos_process_data_area(uint32_t *size_out);
int32_t myrtos_mem_free(void *ptr);
void *myrtos_mem_realloc(void *ptr, uint32_t size);
void myrtos_sleep_tick(void);
extern tlsf_pool_t myrtos_mem_pool;

// The system call numbers come from common/myrtos_abi.h, shared with modules.

#define MCAUSE_INTERRUPT_BIT    0x80000000u
#define MCAUSE_CODE_MASK        0x7fffffffu
#define MCAUSE_ECALL_M          11u
#define MCAUSE_BREAKPOINT        3u
#define MCAUSE_MACHINE_TIMER     7u
#define MCAUSE_MACHINE_EXTERNAL 11u

volatile uint32_t myrtos_ticks = 0;
volatile uint32_t myrtos_trap_count = 0;
volatile uint32_t myrtos_last_mcause = 0;
volatile uint32_t myrtos_last_mepc = 0;

// The return value is the stack pointer to resume. Same in as out means we
// continue in the same process; a different one is a context switch.
uint32_t myrtos_trap_handler(myrtos_frame_t *frame) {
    uint32_t sp = (uint32_t)(uintptr_t)frame;

    myrtos_trap_count++;
    myrtos_last_mcause = frame->mcause;
    myrtos_last_mepc = frame->mepc;

    if (frame->mcause & MCAUSE_INTERRUPT_BIT) {
        if ((frame->mcause & MCAUSE_CODE_MASK) == MCAUSE_MACHINE_TIMER) {
            myrtos_ticks++;
            // A blocked reader is woken here rather than by the driver: TinyUSB
            // delivers into its own buffers, and asking once per tick is both
            // simpler and enough at keyboard speed.
            myrtos_wake_readers();
            myrtos_sleep_tick();
// The interrupt stays pending until mtimecmp moves forward. Without
// this it recurs immediately and the machine does nothing else.
            myrtos_timer_rearm();
// Time slicing: on every tick the next runnable process takes over.
            return myrtos_switch(sp);
        }
        return sp;
    }

    if ((frame->mcause & MCAUSE_CODE_MASK) == MCAUSE_ECALL_M) {
// mepc points at the ecall instruction itself. Without this step
// mret returns to the same instruction and the machine loops.
        frame->mepc += 4;

        switch (frame->a7) {
        case SYS_NULL:
            frame->a0 = 0;
            break;
        case SYS_IO_PUTC:
            myrtos_putc((char)frame->a0);
            frame->a0 = 0;
            break;
        case SYS_OPEN:
            frame->a0 = (uint32_t)myrtos_io_open((const char*)(uintptr_t)frame->a0,
                                                 myrtos_current_pid());
            break;
        case SYS_WRITE: {
            int32_t wpath = (int32_t)frame->a0;
            int32_t wn = myrtos_io_write(wpath, (const uint8_t*)(uintptr_t)frame->a1,
                                         frame->a2, myrtos_current_pid());
            if (wn == 0 && frame->a2 && myrtos_current_pid() != 0) {
                // No room. Same shape as a blocking read: step back onto the
                // ecall and wait, so the call is simply made again with its
                // arguments intact once the device can take something.
                frame->mepc -= 4;
                myrtos_block_on_write(wpath);
                return myrtos_switch(sp);
            }
            frame->a0 = (uint32_t)wn;
            break;
        }
        case SYS_SEND: {
            // No mepc rewind here, unlike a blocking read: the call is not made
            // again. The reply writes its status straight into this frame's a0 and
            // the process resumes as though send had returned normally.
            if (!myrtos_msg_send((int32_t)frame->a0,
                                 (const myrtos_msg_t*)(uintptr_t)frame->a1)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        }
        case SYS_RECEIVE: {
            int32_t from = myrtos_msg_receive((myrtos_msg_t*)(uintptr_t)frame->a0);
            if (from == -1) return myrtos_switch(sp);   // nothing yet; wait
            frame->a0 = (uint32_t)from;                 // -2 = reply first
            break;
        }
        case SYS_REPLY:
            frame->a0 = (uint32_t)myrtos_msg_reply((int32_t)frame->a0);
            break;
        case SYS_REPLYTO:
            frame->a0 = (uint32_t)myrtos_msg_reply_to((int32_t)frame->a0,
                                                      (int32_t)frame->a1);
            break;
        case SYS_PIDOF:
            frame->a0 = (uint32_t)myrtos_find_pid((const char*)(uintptr_t)frame->a0);
            break;
        case SYS_READ: {
            int32_t path = (int32_t)frame->a0;
            int32_t n = myrtos_io_read(path, (uint8_t*)(uintptr_t)frame->a1,
                                       frame->a2, myrtos_current_pid());
            if (n == 0 && myrtos_current_pid() != 0) {
                // Nothing there. Step mepc back onto the ecall and block: when
                // the process runs again it re-executes the call with its
                // arguments still in place, so nothing has to be remembered
                // about a half-finished read.
                frame->mepc -= 4;
                myrtos_block_on_read(path);
                return myrtos_switch(sp);
            }
            frame->a0 = (uint32_t)n;
            break;
        }
        case SYS_EXEC: {
// OS-9's F$Link and F$Fork in one: look the module up, bump
// its link count, and make a process of it. The code is shared --
// only the data area is new.
            const char *want = (const char*)(uintptr_t)frame->a0;
            const char *stored = myrtos_moddir_match(want);
            if (!stored) { frame->a0 = (uint32_t)-1; break; }
            const myrtos_module_header_t *m = myrtos_moddir_link(stored);
// The arguments are passed at creation: they are copied into the
// process's own memory before the frame is built, so a0 can point past them.
            int32_t pid = m ? myrtos_process_create(m, (const char*)(uintptr_t)frame->a1) : -1;
            if (pid >= 0) {
                  myrtos_io_inherit(myrtos_current_pid(), pid);
                  myrtos_cwd_inherit(myrtos_current_pid(), pid);
              }
            frame->a0 = (uint32_t)pid;
            break;
        }
        case SYS_ARGS:
            frame->a0 = myrtos_process_get_args((char*)(uintptr_t)frame->a0, frame->a1);
            break;
        case SYS_FSDIR:
            if (!fs_request(MYRTOS_MSG_FS_DIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_CHDIR:
            if (!fs_request(MYRTOS_MSG_FS_CHDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_GETCWD: {
            char *out = (char*)(uintptr_t)frame->a0;
            const char *cwd = myrtos_cwd_get();
            uint32_t i = 0;
            while (cwd[i] && i + 1 < frame->a1) { out[i] = cwd[i]; i++; }
            out[i] = 0;
            frame->a0 = i;
            break;
        }
        case SYS_MKDIR:
            if (!fs_request(MYRTOS_MSG_FS_MKDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_RMDIR:
            if (!fs_request(MYRTOS_MSG_FS_RMDIR, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_WIFIADDR: {
            myrtos_wifi_req_t req;
            req.index = 0;
            req.buf   = (char*)(uintptr_t)frame->a0;
            req.len   = frame->a1;
            if (!server_request(myrtos_wifi_server_pid(), MYRTOS_MSG_WIFI_ADDR, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        }
        case SYS_WIFIJOIN: {
            // Through the service, like the others: joining waits seconds and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            myrtos_wifi_req_t req;
            req.index = 0;
            req.buf   = (char*)(uintptr_t)frame->a0;
            req.len   = 0;
            if (!server_request(myrtos_wifi_server_pid(), MYRTOS_MSG_WIFI_JOIN, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        }
        case SYS_WIFISCAN: {
            // Through the service rather than here: a scan waits seconds, and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            myrtos_wifi_req_t req;
            req.index = (int32_t)frame->a0;
            req.buf   = (char*)(uintptr_t)frame->a1;
            req.len   = frame->a2;
            if (MYRTOS_MSG_WIFI_SCAN == MYRTOS_MSG_WIFI_VER) {
                req.buf = (char*)(uintptr_t)frame->a0;
                req.len = frame->a1;
            }
            if (!server_request(myrtos_wifi_server_pid(), MYRTOS_MSG_WIFI_SCAN, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        }
        case SYS_WIFIVER: {
            // Through the service rather than here: a scan waits seconds, and
            // waiting inside a trap stops the machine. The request is built on
            // this stack, which is safe because send blocks until the answer.
            myrtos_wifi_req_t req;
            req.index = (int32_t)frame->a0;
            req.buf   = (char*)(uintptr_t)frame->a1;
            req.len   = frame->a2;
            if (MYRTOS_MSG_WIFI_VER == MYRTOS_MSG_WIFI_VER) {
                req.buf = (char*)(uintptr_t)frame->a0;
                req.len = frame->a1;
            }
            if (!server_request(myrtos_wifi_server_pid(), MYRTOS_MSG_WIFI_VER, &req)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        }
        case SYS_MOUNT:
            if (!fs_request(MYRTOS_MSG_FS_MOUNT, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_FOREGRND:
            frame->a0 = (uint32_t)myrtos_io_set_foreground(
                (int32_t)frame->a0, (int32_t)frame->a1, myrtos_current_pid());
            break;
        case SYS_KILL: {
            int32_t victim = (int32_t)frame->a0;
            // Killing yourself is exiting, and exiting never returns.
            if (victim == myrtos_current_pid()) {
                myrtos_process_exit();
                return myrtos_switch(sp);
            }
            frame->a0 = (uint32_t)myrtos_process_kill(victim);
            break;
        }
        case SYS_READABLE:
            frame->a0 = (uint32_t)myrtos_io_readable_count((int32_t)frame->a0,
                                                           myrtos_current_pid());
            break;
        case SYS_CONFONT:
            // Cheap enough to serve here: it records which font is wanted and
            // fills in a struct of six bytes. The console's own thread does the
            // work, on its own time, once it has drawn what was already queued.
            frame->a0 = (uint32_t)myrtos_console_select_font(
                (int32_t)frame->a0, (myrtos_confont_t*)(uintptr_t)frame->a1,
                frame->a2 != 0);
            break;
        case SYS_FSREAD:
            if (!fs_request(MYRTOS_MSG_FS_READ, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_FSWRITE:
            if (!fs_request(MYRTOS_MSG_FS_WRITE, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_FSREMOVE:
            if (!fs_request(MYRTOS_MSG_FS_REMOVE, (void*)(uintptr_t)frame->a0)) {
                frame->a0 = (uint32_t)-1;
                break;
            }
            return myrtos_switch(sp);
        case SYS_CLOSE:
            frame->a0 = (uint32_t)myrtos_io_close((int32_t)frame->a0, myrtos_current_pid());
            break;
        case SYS_MODDIR: {
            const myrtos_module_entry_t *e = myrtos_moddir_entry(frame->a0);
            if (!e) { frame->a0 = (uint32_t)-1; break; }
            myrtos_modinfo_t *out = (myrtos_modinfo_t*)(uintptr_t)frame->a1;
            for (int i = 0; i < 12; i++) out->name[i] = e->name[i];
            out->links = e->links;
            out->revision = e->header->revision;
            out->size = e->header->module_size;
            frame->a0 = 0;
            break;
        }
        case SYS_MEMINFO:
            if (frame->a0 == MYRTOS_MEM_BULK_FREE) {
                frame->a0 = myrtos_bulk_pool
                          ? (uint32_t)myrtos_tlsf_largest_free(myrtos_bulk_pool) : 0;
                break;
            }
            if (frame->a0 == MYRTOS_MEM_BULK_SIZE) {
                frame->a0 = myrtos_bulk_pool ? (uint32_t)myrtos_psram_bytes() : 0;
                break;
            }
            frame->a0 = (frame->a0 == MYRTOS_MEM_PROCESSES)
                ? myrtos_process_count()
                : (uint32_t)myrtos_tlsf_largest_free(myrtos_mem_pool);
            break;
        case SYS_DATAAREA:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_process_data_area(
                            (uint32_t*)(uintptr_t)frame->a0);
            break;
        case SYS_ALLOCBULK:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_mem_alloc_bulk(frame->a0);
            break;
        case SYS_ALLOC:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_mem_alloc(frame->a0);
            break;
        case SYS_FREE:
            frame->a0 = (uint32_t)myrtos_mem_free((void*)(uintptr_t)frame->a0);
            break;
        case SYS_REALLOC:
            frame->a0 = (uint32_t)(uintptr_t)myrtos_mem_realloc(
                            (void*)(uintptr_t)frame->a0, frame->a1);
            break;
        case SYS_BOOTSEL:
            myrtos_reboot_bootsel();    // does not return
            break;
        case SYS_PSINFO:
            frame->a0 = (uint32_t)myrtos_process_info(frame->a0,
                            (myrtos_psinfo_t*)(uintptr_t)frame->a1);
            break;
        case SYS_TICKS:
            frame->a0 = (uint32_t)myrtos_ticks;
            break;
        case SYS_SETPRIO:
            frame->a0 = myrtos_set_priority(frame->a0);
            break;
        case SYS_SLEEP: {
            uint32_t ms = frame->a0;                // read before a0 is the result
            frame->a0 = 0;
            if (myrtos_current_pid() == 0) break;   // the kernel does not sleep
            if (ms == 0) return myrtos_switch(sp);  // zero is a yield
            myrtos_sleep_begin(ms);
            return myrtos_switch(sp);
        }
        case SYS_WAIT:
            if (myrtos_block_on_child((int32_t)frame->a0)) {
                frame->a0 = 0;
                return myrtos_switch(sp);
            }
            frame->a0 = 0;      // already gone; nothing to wait for
            break;
        case SYS_EXIT:
// The process is not to be resumed, so we switch away at once.
            myrtos_process_exit();
            return myrtos_switch(sp);
        default:
            // The number matters: without it the message says only that
            // something is wrong, which cost an hour when a stale kernel met a
            // module built against a newer one.
            myrtos_print("*** MYRTOS: unknown system call ");
            myrtos_print_u32(frame->a7);
            myrtos_print(" ***\n");
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
    // it. Compressed ebreak is two bytes and the wide one is four; the low two
    // bits of the instruction say which.
    //
    // The tally matters as much as the step. A stack that asserts on every poll
    // would otherwise look like a machine that works, so the first one is
    // printed and all of them are counted.
    if (frame->mcause == MCAUSE_BREAKPOINT) {
        if (!myrtos_asserts_seen) {
            myrtos_crash_note(MYRTOS_CRASH_ASSERT, frame->mepc, 0, 0);
            myrtos_print("\n*** MYRTOS: assertion at ");
            myrtos_print_u32(frame->mepc);
            myrtos_print(", stepped over ***\n");
        }
        myrtos_asserts_seen++;
        myrtos_assert_last = frame->mepc;
        uint16_t insn = *(const uint16_t *)(uintptr_t)frame->mepc;
        frame->mepc += ((insn & 3u) == 3u) ? 4u : 2u;
        return sp;
    }

    // Write it down before saying anything, because saying it goes through the
    // console -- and a fault this early is usually a fault on the way to having
    // one. What the probe reads must not depend on the screen ever working.
    myrtos_crash_note(MYRTOS_CRASH_TRAP, frame->mepc, frame->mcause, frame->mtval);

    myrtos_print("\n*** MYRTOS TRAP: unhandled exception ***\n");
    myrtos_print("  mepc ");   myrtos_print_u32(frame->mepc);
    myrtos_print("  mcause "); myrtos_print_u32(frame->mcause);
    myrtos_print("  mtval ");  myrtos_print_u32(frame->mtval);
    myrtos_print("\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}
