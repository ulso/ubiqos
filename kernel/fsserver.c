// The filesystem as a service process.
//
// Every filesystem call used to run inside the caller's trap, with interrupts
// off for its whole duration. Reading a few sectors survives that; zeroing a
// cluster for a new directory does not -- eight or more SPI transactions, some
// milliseconds, during which no process runs at all. The USB task is a process,
// the keyboard is bit-banged on PIO and polled every millisecond, and it was
// simply lost. That was the fourth appearance of one root cause: long work in a
// trap.
//
// So the work moves out. A filesystem call becomes a message to this process,
// which does the same thing in process context where the scheduler can take the
// processor away from it. The caller blocks in send until the reply, which is
// what it did before anyway -- a read is synchronous by nature and the client
// has nothing to do until it has its bytes.
//
// The request itself is never copied. It points into the calling process's own
// memory, which cannot change because the process is stopped, so this reads the
// same struct the utility filled in.

#include <stdint.h>
#include <stdbool.h>
#include "../common/modules.h"
#include "fat32.h"
#include "usbdev.h"
#include "sdcard.h"
#include "moddir.h"
#include "tlsf.h"

void myrtos_print(const char *s);
int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);
const char *myrtos_cwd_of(int32_t pid);
static bool card_bring_up(bool try_sdio);
bool myrtos_cwd_set_of(int32_t pid, const char *abs);

static int32_t server_pid = -1;

int32_t myrtos_fs_server_pid(void) { return server_pid; }

// Build an absolute, tidied path from one as the client typed it. Relative
// paths start at the client's current directory -- the client's, not the
// server's -- and "." and ".." are folded away here so that what the shell
// shows and what the filesystem walks are the same thing.
static void make_abs(int32_t pid, const char *in, char *out, uint32_t out_len) {
    char buf[128];
    uint32_t n = 0;

    if (!in) in = "";
    if (in[0] != '/') {
        const char *cwd = myrtos_cwd_of(pid);
        while (cwd[n] && n < sizeof(buf) - 2) { buf[n] = cwd[n]; n++; }
        if (n == 0 || buf[n - 1] != '/') buf[n++] = '/';
    } else {
        buf[n++] = '/';
    }

    uint32_t i = 0;
    while (in[i] && n < sizeof(buf) - 2) {
        if (in[i] == '/') { i++; continue; }

        char comp[16];
        uint32_t c = 0;
        while (in[i] && in[i] != '/' && c < sizeof(comp) - 1) comp[c++] = in[i++];
        comp[c] = 0;
        while (in[i] && in[i] != '/') i++;          // a longer component is cut

        if (comp[0] == '.' && comp[1] == 0) continue;
        if (comp[0] == '.' && comp[1] == '.' && comp[2] == 0) {
            if (n > 1) { n--; while (n > 1 && buf[n - 1] != '/') n--; }
            continue;
        }
        for (uint32_t k = 0; k < c && n < sizeof(buf) - 2; k++) buf[n++] = comp[k];
        buf[n++] = '/';
    }

    if (n > 1 && buf[n - 1] == '/') n--;            // no trailing slash except at the root
    buf[n] = 0;

    uint32_t k = 0;
    while (buf[k] && k < out_len - 1) { out[k] = buf[k]; k++; }
    out[k] = 0;
}

static int32_t handle(int32_t from, const myrtos_msg_t *m) {
    char abs[128];

    switch (m->type) {
    case MYRTOS_MSG_FS_READ: {
        const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)m->data;
        make_abs(from, r->name, abs, sizeof(abs));
        return myrtos_fat_read_at(abs, r->offset, r->buf, r->len);
    }
    case MYRTOS_MSG_FS_WRITE: {
        const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)m->data;
        make_abs(from, r->name, abs, sizeof(abs));
        return myrtos_fat_write_at(abs, r->offset, r->buf, r->len);
    }
    case MYRTOS_MSG_FS_REMOVE:
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        return myrtos_fat_remove(abs) ? 0 : -1;
    case MYRTOS_MSG_FS_MKDIR:
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        return myrtos_fat_mkdir(abs) ? 0 : -1;
    case MYRTOS_MSG_FS_RMDIR:
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        return myrtos_fat_rmdir(abs) ? 0 : -1;
    case MYRTOS_MSG_FS_MOUNT:
        // Talking to the card can take a second when there is none in the slot,
        // which is a reason for this to be asked for rather than attempted
        // behind every failed listing.
        return card_bring_up((uintptr_t)m->data == MYRTOS_MOUNT_SDIO) ? 0 : -1;
    case MYRTOS_MSG_FS_DIR: {
        const myrtos_fs_dir_t *d = (const myrtos_fs_dir_t*)m->data;
        make_abs(from, d->path, abs, sizeof(abs));
        return myrtos_fat_stat_nth(abs, d->index, d->name, d->size);
    }
    case MYRTOS_MSG_FS_CHDIR: {
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        // The directory has to exist, and listing its first entry is the
        // cheapest way to ask: even an empty one still has "." in it, so a real
        // directory always answers. The root is taken on trust.
        char name[12];
        uint32_t size = 0;
        if (abs[1] && myrtos_fat_stat_nth(abs, 0, name, &size) < 0) return -1;
        return myrtos_cwd_set_of(from, abs) ? 0 : -1;
    }
    default:
        return -1;
    }
}

// --- THE CARD ------------------------------------------------------------
// All of it, here, and none of it in main. Two reasons, and the second is the
// one that cost a boot.
//
// The card must be asked for SDIO before anything speaks SPI to it: it latches
// into SPI mode the moment it is addressed that way and stays there until the
// power is cut. So whoever brings it up has to be the first to touch it, and
// that used to be main.
//
// And it cannot be main, because the driver's waits are unbounded -- upstream
// marks them "todo not forever" -- and main runs before the scheduler. A hang
// there takes the console and USB with it and leaves the BOOTSEL button. Here
// it costs this one process, and the shell, the screen and the keyboard carry
// on, which is the difference between a fault you can look at and a dark board.
extern tlsf_pool_t myrtos_mem_pool;
extern tlsf_pool_t myrtos_bulk_pool;
void myrtos_print_u32(uint32_t v);

// A module read from the card is copied into RAM and stays there, so the buffer
// it arrives through need not: 32 kB touched once per module, from PSRAM when
// there is any, and handed straight back.
static void register_card_modules(void) {
    const uint32_t staging_size = 32 * 1024;
    tlsf_pool_t pool = myrtos_bulk_pool ? myrtos_bulk_pool : myrtos_mem_pool;
    uint8_t *staging = myrtos_tlsf_malloc(pool, staging_size);
    if (!staging) {
        myrtos_print("SD: no buffer to read modules into\n");
        return;
    }

    char name[12];
    for (uint32_t i = 0; myrtos_fat_find_nth("MOD", i, name); i++) {
        int32_t n = myrtos_fat_read_file(name, staging, staging_size);
        if (n <= 0) continue;
        if (myrtos_moddir_add_copy(staging, (uint32_t)n, name)) {
            myrtos_print("Registered ");
            myrtos_print(name);
            myrtos_print(" from card, ");
            myrtos_print_u32((uint32_t)n);
            myrtos_print(" bytes\n");
        }
    }
    myrtos_tlsf_free(pool, staging);
}

// Which bus, asked for by name. The two are not interchangeable and cannot be
// tried in turn: the card latches into SPI as soon as it is addressed that way
// and stays there until the power is cut, so a failed SDIO attempt after a
// successful SPI mount is not a card that refused -- it is a card that can no
// longer hear the question. Falling back automatically therefore spends the one
// chance at SDIO on the first `ls` anyone types.
//
// And SDIO is not free to attempt. It configures DMA channels 8-11 by hardcoded
// number without claiming them, and reprograms PIO1; both belong to the video
// chain. The screen goes black on `mount` and stays black until a reset, every
// time, and that is still not understood. So it is behind a word the user has
// to type, and plain `mount` is the safe one.
static bool card_bring_up(bool try_sdio) {
    if (try_sdio) {
        if (!myrtos_sd_try_sdio() || !myrtos_fat_mount()) {
            myrtos_print("SD: no SDIO -- no card, or SPI was asked for first\n");
            return false;
        }
        myrtos_print("SD: four-bit SDIO\n");
    } else {
        if (!myrtos_sd_init() || !myrtos_fat_mount()) {
            myrtos_print("SD: no card, or not FAT32\n");
            return false;
        }
        myrtos_print("SD: SPI\n");
    }
    register_card_modules();
    return true;
}

static void fs_thread(void) {
    // Nothing touches the card here, and that is deliberate. Mounting it at
    // startup would latch it into SPI before anyone could ask for SDIO, and it
    // would spend the driver's unbounded waits on a machine that has just come
    // up. The card is mounted when someone says `mount`, and not before.
    for (;;) {
        myrtos_msg_t m;
        int32_t from = myrtos_receive(&m);
        if (from < 0) continue;              // -2 would mean an unanswered one
        myrtos_reply(handle(from, &m));
    }
}

void myrtos_fs_start_server(void) {
    // Below the USB task and the console, above a shell. It holds the card's
    // only buffers, so there is exactly one of it and no locking to get wrong.
    server_pid = myrtos_kernel_thread(fs_thread, 4096, MYRTOS_PRIO_FS);
    if (server_pid < 0) myrtos_print("FS: could not start its service process\n");
}
