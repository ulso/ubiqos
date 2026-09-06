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
#include "vfs.h"
#include "io.h"
#include "usbdev.h"
#include "sdcard.h"
#include "moddir.h"
#include "tlsf.h"

void myrtos_print(const char *s);
// No header declares this one; main.c reaches for it the same way.
int32_t myrtos_process_create(const myrtos_module_header_t *module_ptr, const char *args);
void    myrtos_cwd_inherit(int32_t parent, int32_t child);
int32_t myrtos_kernel_thread(void (*entry)(void), uint32_t stack_bytes, uint32_t priority);
const char *myrtos_cwd_of(int32_t pid);
static bool card_bring_up(bool try_sdio);
static bool card_mounted;
static bool load_module_from_card(const char *name);
bool myrtos_msc_hand_over(void);
void myrtos_msc_take_back(void);
bool myrtos_msc_host_has_card(void);
// Which pool a module belongs in: SRAM if it is real-time, PSRAM otherwise.
tlsf_pool_t myrtos_pool_for(const myrtos_module_header_t *m);
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

        // As long as a name may now be: a component cut short here would turn
        // "/sd/a-long-name.wasm" into a path to something that is not there.
        char comp[MYRTOS_DIRNAME_MAX];
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

// A path names its volume first -- "/sd/docs/x" -- so every request begins by
// splitting that off and finding who owns the rest. A volume that is not
// mounted, and the root itself, own no files: both come back as null here and
// the request is refused, which is the same answer a missing file has always
// given.
#define VOLUME_OR_FAIL(op)                                       \
    const char *rest;                                            \
    const myrtos_fsops_t *ops = myrtos_vfs_split(abs, &rest);    \
    if (!ops || !ops->op) return -1

// A card that has stopped answering loses its volume.
//
// Without this the card stayed mounted and every path under /sd went on being
// accepted and failing -- and pulling a card out of a running board produced a
// screen full of driver complaints rather than "no such directory". Now the
// first failure takes /sd out of the volume table, so the answer is the one the
// user can act on, and `mount` is what puts it back.
//
// Checked here rather than in the driver because unmounting is the filesystem's
// business, and this is the one place every filesystem request passes through.
static void drop_card_if_dead(void) {
    if (!card_mounted || !myrtos_sd_failed()) return;
    myrtos_vfs_remove("sd");
    myrtos_sd_forget();          // so the next mount starts from CMD0
    card_mounted = false;
    myrtos_print("SD: the card stopped answering; /sd is unmounted\n");
}

static int32_t handle(int32_t from, const myrtos_msg_t *m) {
    char abs[128];

    drop_card_if_dead();

    switch (m->type) {
    case MYRTOS_MSG_FS_READ: {
        const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)m->data;
        make_abs(from, r->name, abs, sizeof(abs));
        VOLUME_OR_FAIL(read_at);
        return ops->read_at(rest, r->offset, r->buf, r->len);
    }
    case MYRTOS_MSG_FS_WRITE: {
        const myrtos_fs_io_t *r = (const myrtos_fs_io_t*)m->data;
        make_abs(from, r->name, abs, sizeof(abs));
        VOLUME_OR_FAIL(write_at);
        return ops->write_at(rest, r->offset, r->buf, r->len);
    }
    case MYRTOS_MSG_FS_REMOVE: {
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        VOLUME_OR_FAIL(remove);
        return ops->remove(rest) ? 0 : -1;
    }
    case MYRTOS_MSG_FS_MKDIR: {
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        VOLUME_OR_FAIL(mkdir);
        return ops->mkdir(rest) ? 0 : -1;
    }
    case MYRTOS_MSG_FS_RMDIR: {
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        VOLUME_OR_FAIL(rmdir);
        return ops->rmdir(rest) ? 0 : -1;
    }
    case MYRTOS_MSG_FS_OPEN: {
        const myrtos_fs_open_t *o = (const myrtos_fs_open_t*)m->data;
        make_abs(from, o->name, abs, sizeof(abs));
        const char *rest;
        const myrtos_fsops_t *ops = myrtos_vfs_split(abs, &rest);
        if (!ops) return -1;

        uint32_t size = 0;
        int32_t attr = ops->stat ? ops->stat(rest, &size) : -1;
        bool exists = attr >= 0;
        bool creating = (o->flags & (MYRTOS_O_CREAT | MYRTOS_O_TRUNC)) != 0;

        // A directory is not a file and must not open as one. It used to: the
        // check was only that the name existed, so opening a directory gave a
        // descriptor onto something with no bytes in it, and writing through
        // that descriptor was a question nobody had an answer for. myrtos has a
        // separate way to read a directory -- the nth entry -- and that is the
        // only way it can be read.
        if (exists && (attr & MYRTOS_ATTR_DIRECTORY)) return -1;

        // Reading something that is not there is an error, and this is the
        // moment to say so: leaving it to the first read means the caller has a
        // descriptor onto nothing and finds out later, which is how cat had to
        // tell a missing file from an empty one by the sign of a return value.
        if (!exists && !creating) return -1;

        if ((o->flags & MYRTOS_O_TRUNC) && exists && ops->remove) {
            ops->remove(rest);
            size = 0;
        }

        int32_t fd = myrtos_io_open_file(abs, from);
        if (fd < 0) return -1;
        // Appending is a position, and setting it here means the caller never
        // holds a descriptor that is pointing at the wrong place.
        if ((o->flags & MYRTOS_O_APPEND) && size)
            myrtos_io_file_seek(fd, from, (int32_t)size, MYRTOS_SEEK_SET);
        return fd;
    }
    // Seek from the end, sent here because it needs the file's length. Asked
    // of the filesystem at this moment rather than remembered from open: a
    // descriptor that has been written through is longer than it was, and a
    // cached length would send the caller to the wrong place with no sign.
    case MYRTOS_MSG_FS_SEEK: {
        const myrtos_fs_seek_t *k = (const myrtos_fs_seek_t*)m->data;
        const char *stored;
        uint32_t pos = 0;
        if (!myrtos_io_file_at(k->fd, from, &stored, &pos)) return -1;

        const char *rest;
        const myrtos_fsops_t *ops = myrtos_vfs_split(stored, &rest);
        if (!ops || !ops->stat) return -1;

        uint32_t size = 0;
        if (ops->stat(rest, &size) < 0) return -1;

        return myrtos_io_file_seek(k->fd, from, (int32_t)size + k->offset,
                                   MYRTOS_SEEK_SET);
    }
    case MYRTOS_MSG_FS_FDIO: {
        const myrtos_fs_fdio_t *r = (const myrtos_fs_fdio_t*)m->data;
        const char *stored;
        uint32_t pos = 0;
        if (!myrtos_io_file_at(r->fd, from, &stored, &pos)) return -1;

        // The stored path is already absolute -- it was resolved when the
        // descriptor was opened, against the working directory of that moment.
        // A process that has since done cd does not move its open files.
        const char *rest;
        const myrtos_fsops_t *ops = myrtos_vfs_split(stored, &rest);
        if (!ops) return -1;

        int32_t n;
        if (r->write) {
            if (!ops->write_at) return -1;
            n = ops->write_at(rest, pos, r->buf, r->len);
        } else {
            if (!ops->read_at) return -1;
            n = ops->read_at(rest, pos, r->buf, r->len);
        }
        if (n > 0) myrtos_io_file_advance(r->fd, from, (uint32_t)n);
        return n;
    }
    case MYRTOS_MSG_FS_STAT: {
        const myrtos_fs_stat_t *r = (const myrtos_fs_stat_t*)m->data;
        make_abs(from, r->name, abs, sizeof(abs));
        // The machine root is a directory that no volume owns, and saying so is
        // better than saying it does not exist.
        if (!abs[1]) { if (r->size) *r->size = 0; return MYRTOS_ATTR_DIRECTORY; }
        VOLUME_OR_FAIL(stat);
        return ops->stat(rest, r->size);
    }
    // Handing the card to the host, and taking it back. It goes through here
    // rather than straight to the USB code so that it queues behind whatever
    // the server was doing: a reply means the last write has landed, which is
    // the difference between a clean volume and one the host will find halfway
    // through a directory update.
    // Making a process, which is here rather than in the trap that asked for it
    // because a single-instance module has to be copied to the address it was
    // linked for, and that is a third of a megabyte from flash into PSRAM for
    // the interpreter. In a trap the copy runs with interrupts off and the USB
    // bus dies underneath it; here the rest of the machine keeps running.
    //
    // This server rather than a new one, because it is already the place where
    // the slow parts of starting something live: a module read off the card
    // arrives through MYRTOS_MSG_FS_LOADMOD a few lines from here.
    //
    // The descriptors and the working directory are inherited from the SENDER,
    // not from this thread. It is the shell that is starting a command, and it
    // is the shell's stdin the command should read.
    case MYRTOS_MSG_FS_EXEC: {
        const myrtos_fs_exec_t *e = (const myrtos_fs_exec_t*)m->data;
        int32_t pid = myrtos_process_create(e->module, e->args);
        if (pid >= 0) {
            myrtos_io_inherit(from, pid);
            myrtos_cwd_inherit(from, pid);
        }
        return pid;
    }

    case MYRTOS_MSG_FS_USBDISK: {
        uintptr_t what = (uintptr_t)m->data;
        bool give_away = (what == 1);
        bool force     = (what == 2);
        if (give_away) {
            if (!card_mounted) return -1;
            if (!myrtos_msc_hand_over()) return -1;
            myrtos_vfs_remove("sd");
            card_mounted = false;
            myrtos_print("USB disk: the card is the host's now; eject it there\n");
            return 0;
        }
        // Taking it back is not a mount. The card was never lost -- only lent --
        // so it is still on whatever bus it was, still initialised, and the
        // driver's state is intact. Re-running the bring-up would call
        // myrtos_sd_init, which speaks SPI to it, and a card latches into SPI
        // the moment it is addressed that way and stays there until the power
        // is cut. Lending the card out would then cost four-bit SDIO for the
        // rest of the session, which is a steep price for copying a file.
        //
        // What does have to happen is re-reading the filesystem, because the
        // host has been writing to it: the boot sector, the FAT and every
        // cached thing this side believed about the directory are out of date.
        // Refuse while the host still has it, because it means no eject has
        // arrived. A host that ejects properly sends START_STOP_UNIT, which
        // clears this by itself -- measured 2 Sep 2026: macOS sends no
        // PREVENT/ALLOW and no SYNCHRONIZE CACHE, but it does send the eject
        // when the volume is ejected. So the flag still being set says the
        // host is holding a mounted volume, and taking the card from under it
        // leaves it hung against a device that has stopped answering. Finder
        // hangs, diskutil hangs, and the way out is to give the card back.
        //
        // Not a hard refusal: a host that has gone away -- an unplugged cable,
        // a sleeping machine -- will never send anything, and then this is the
        // only way back. `usbdisk force` says so deliberately.
        if (myrtos_msc_host_has_card() && !force) {
            myrtos_print("USB disk: the host has not ejected it\n");
            return -2;
        }
        myrtos_msc_take_back();
        if (!myrtos_fat_mount()) {
            myrtos_print("USB disk: the card came back unreadable\n");
            return -1;
        }
        if (!card_mounted && !myrtos_vfs_add("sd", &myrtos_fat_ops)) {
            myrtos_print("USB disk: no room in the volume table\n");
            return -1;
        }
        card_mounted = true;
        return 0;
    }
    case MYRTOS_MSG_FS_LOADMOD:
        return load_module_from_card((const char*)m->data) ? 0 : -1;
    case MYRTOS_MSG_FS_MOUNT:
        // A distinct answer, so `mount` can say which of the two it is. Saying
        // "no card, or not FAT32" about a card the host is holding sends the
        // reader looking for the wrong problem entirely.
        if (myrtos_msc_host_has_card()) return -2;
        // Talking to the card can take a second when there is none in the slot,
        // which is a reason for this to be asked for rather than attempted
        // behind every failed listing.
        return card_bring_up((uintptr_t)m->data == MYRTOS_MOUNT_SDIO) ? 0 : -1;
    case MYRTOS_MSG_FS_DIR: {
        const myrtos_fs_dir_t *d = (const myrtos_fs_dir_t*)m->data;
        make_abs(from, d->path, abs, sizeof(abs));
        // The root is owned by nobody, so listing it lists the volumes. That is
        // the one directory no filesystem can answer for.
        if (!abs[1]) return myrtos_vfs_root_nth(d->index, d->name, d->size);
        VOLUME_OR_FAIL(stat_nth);
        return ops->stat_nth(rest, d->index, d->name, d->size);
    }
    case MYRTOS_MSG_FS_CHDIR: {
        make_abs(from, (const char*)m->data, abs, sizeof(abs));
        // The directory has to exist, and listing its first entry is the
        // cheapest way to ask: even an empty one still has "." in it, so a real
        // directory always answers. The root is taken on trust.
        char name[MYRTOS_DIRNAME_MAX];
        uint32_t size = 0;
        if (abs[1]) {
            const char *rest;
            const myrtos_fsops_t *ops = myrtos_vfs_split(abs, &rest);
            if (!ops) return -1;
            // A volume's own root is taken on trust, as the machine root always
            // was: an empty FAT root has no entries to prove itself with.
            if (rest[1] && (!ops->stat_nth || ops->stat_nth(rest, 0, name, &size) < 0))
                return -1;
        }
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

// One module off the card, by name, because something is trying to run it.
//
// Nothing is read at mount. That was the other way round until 2 Sep 2026:
// every .MOD on the card was read at mount and kept for the session, run or
// not. What that bought was the revision check -- a newer build on the card
// superseding the one in flash -- and Ulf's recollection of OS-9, where the
// revision number was there so a later version could be pushed into an EPROM
// socket, is that nothing scanned a disk to patch ROM modules. It does not any
// more here either. Flash modules already worked this way: adopt_from_flash
// gives one a directory entry when something runs it and drops it afterwards.
//
// The file is read twice on purpose. The first read is the header alone, which
// says how long the module is and whether it wants real time -- and therefore
// which pool it belongs in. Reading the header first means the body is
// allocated once, at its own size, in the right place. The old scan used a
// fixed 32 kB staging buffer for every module regardless.
static bool load_module_from_card(const char *name) {
    const char *vol = "";
    const myrtos_fsops_t *ops = myrtos_vfs_module_volume(&vol);
    if (!ops || !ops->stat || !ops->read_at) return false;

    // The name as given, and then the name with .mod after it.
    //
    // There is no extension requirement any more. A module file may be called
    // whatever the module is called, and .mod is a habit the card already has
    // rather than a rule -- so both are tried, the bare name first.
    //
    // What decides whether a file holds a module is the file, not its name:
    // four bytes are read and compared with the sync code before anything else
    // happens. That is the check the extension was standing in for, and it is
    // the real one -- a file called foo.mod containing a photograph was, until
    // now, read as far as a header before anybody noticed.
    //
    // A name too long to be a filename is refused rather than cut. Cutting is
    // what made "hibouair&" -- a mistyped command line, with the ampersand
    // meant for the shell -- load hibouair.mod and start the scanner, with the
    // log reading "Loaded hibouair& from /sd" as though it had worked.
    char file[MYRTOS_DIRNAME_MAX + 8];
    uint32_t n = 0;
    while (name[n]) {
        if (n >= MYRTOS_NAME_LEN - 1) return false;
        file[n] = name[n];
        n++;
    }
    if (!n) return false;
    file[n] = 0;

    uint32_t sync = 0;
    if (ops->read_at(file, 0, (uint8_t*)&sync, 4) != 4 || sync != MYRTOS_SYNC_CODE) {
        file[n] = '.'; file[n+1] = 'm'; file[n+2] = 'o'; file[n+3] = 'd'; file[n+4] = 0;
        if (ops->read_at(file, 0, (uint8_t*)&sync, 4) != 4 || sync != MYRTOS_SYNC_CODE)
            return false;
    }

    uint32_t size = 0;
    if (ops->stat(file, &size) < 0 || !size) return false;

    myrtos_module_header_t hdr;
    if (ops->read_at(file, 0, (uint8_t*)&hdr, sizeof hdr) != (int32_t)sizeof hdr)
        return false;
    if (hdr.module_size > size) return false;      // a header that outruns its file

    tlsf_pool_t pool = myrtos_pool_for(&hdr);
    uint8_t *image = myrtos_tlsf_malloc(pool, hdr.module_size);
    if (!image) { myrtos_print("SD: no room for module\n"); return false; }

    bool ok = ops->read_at(file, 0, image, hdr.module_size) == (int32_t)hdr.module_size
              && myrtos_moddir_add_image(image, hdr.module_size, name);
    if (!ok) {
        myrtos_tlsf_free(pool, image);
        return false;
    }
    myrtos_print("Loaded ");
    myrtos_print(name);
    myrtos_print(" from /");
    myrtos_print(vol);
    myrtos_print("\n");
    return true;
}

// Which bus, asked for by name. The two are not interchangeable and cannot be
// tried in turn: the card latches into SPI as soon as it is addressed that way
// and stays there until the power is cut, so a failed SDIO attempt after a
// successful SPI mount is not a card that refused -- it is a card that can no
// longer hear the question. Falling back automatically therefore spends the one
// chance at SDIO on the first `ls` anyone types.
//
// SDIO used to black the screen out on every attempt, and that is fixed: it was
// spoop() in the vendored driver reprogramming DMA channel 3, which the video
// chain owns. With it gone, the DMA read address was sampled three times across
// a `mount sdio` and was walking the framebuffer each time. The bystanders the
// old comment here accused are innocent -- the driver's own channels are 8-11
// and its PIO block is PIO1, while video runs on DMA 1-3 and HSTX and the USB
// host on PIO0, so none of them overlap.
//
// What is still true is that those four channels are hardcoded and never
// claimed, so the DMA allocator will hand 8-11 out to whoever asks next and
// nothing will complain until both are running.
//
// So SDIO stays behind a word the user types, but for the reason above this
// paragraph rather than this one: a wrong guess costs the card's one chance.
static bool card_bring_up(bool try_sdio) {
    // Not while the host has it. Without this the whole arrangement is
    // decoration: `usbdisk` then `mount` puts the card under two filesystems
    // that each cache its directory and free-cluster map, and the volume is
    // ruined by whichever writes second.
    //
    // This is not hypothetical. The first end-to-end test went usbdisk, copy a
    // file on the Mac, eject, mount -- and the mount succeeded even though the
    // eject had never reached the board. macOS unmounted the volume and stopped
    // asking, without sending START_STOP_UNIT, so tud_msc_start_stop_cb never
    // ran and the card was still the host's as far as this was concerned.
    //
    // Which makes `usbdisk off` the ordinary way back rather than the exception:
    // an eject is a courtesy the host may or may not extend, and this cannot be
    // built on it.

    // What the detect pin says, reported and not acted on. See the note on
    // myrtos_sd_present: with a card in the slot it reads as empty, so gating
    // the mount on it stopped the machine mounting a card that was there.
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
    // It becomes /sd. The name is the volume's, not the filesystem's: a
    // LittleFS partition or a USB stick would come in the same way under its
    // own name, and nothing above here would know the difference.
    if (!myrtos_vfs_add("sd", &myrtos_fat_ops)) {
        myrtos_print("SD: no room in the volume table\n");
        return false;
    }
    card_mounted = true;
    return true;
}

// The boot script, run once, by a shell like any other. It is a shell rather
// than something new because the shell already knows how to run a line, and
// its stdin is already a descriptor: point path 0 at a file instead of a
// console and the same loop reads a script. "script" tells it to keep quiet --
// no banner, no prompt, and above all no terminal-width probe, which would eat
// the first 150 ms of the file waiting for a cursor report.
//
// It is started, not waited for. The system is up either way, and a script that
// blocks holds up nothing but itself.
#define STARTUP_PATH "/sd/startup"

static void run_startup_script(void) {
    uint32_t size = 0;
    if (myrtos_fat_stat("startup", &size) < 0) return;   // no script, nothing to say

    const char *sh = myrtos_moddir_match("sh");
    const myrtos_module_header_t *m = sh ? myrtos_moddir_link(sh) : 0;
    int32_t pid = m ? myrtos_process_create(m, "script") : -1;
    if (pid < 0) { myrtos_print("startup: no shell to run it\n"); return; }

    // A new process has no paths, so the file lands on 0 -- but take the
    // number the call gives rather than trusting that, and put it on stdin.
    int32_t in = myrtos_io_open_file(STARTUP_PATH, pid);
    if (in < 0) { myrtos_print("startup: could not open " STARTUP_PATH "\n"); return; }
    if (in != MYRTOS_STDIN) myrtos_io_dup(in, MYRTOS_STDIN, pid);

    const char *console = myrtos_io_has_device("con") ? "con" : "usb";
    myrtos_io_open_as(console, pid, MYRTOS_STDOUT);
    myrtos_io_open_as(console, pid, MYRTOS_STDERR);

    myrtos_print("Running " STARTUP_PATH "\n");
}

static void fs_thread(void) {
    // The card is brought up here, in a process, and this is the only place it
    // can be. Not in main: that runs before the scheduler, so a driver that
    // stalls takes the console and USB with it and the BOOTSEL button is the
    // way back. Here a stall costs one process.
    //
    // Over SPI, and not over SDIO, however tempting the speed is.
    //
    // This did auto-mount SDIO for one build, and it worked until something
    // wrote. Reading over four bits is proven -- 100000 bytes verified byte for
    // byte -- but nothing had ever written over it, because until this function
    // existed the default bus was SPI and every write ever tested went that
    // way. The first `echo > /sd/startup` on SDIO wedged the data state machine
    // and the driver printed "gave up waiting" onto the screen for as long as
    // the board was powered: the retry above it never stops, and the filesystem
    // server at priority 22 starves the shell at 16, so there is no way to type
    // anything at a board that is still running.
    //
    // So the automatic bus is the one that is proven in both directions. `mount
    // sdio` still exists for reading, and it is worth having -- but it must not
    // be what a machine picks for itself before anyone has asked for it.
    // SPI, because it is the bus that can be written to. SDIO reads faster and
    // is reachable with `mount sdio` after a power cycle, but a machine must
    // not choose for itself a bus on which the first write wedges the card.
    // SDIO first, SPI second. The order is the only one that works: a card
    // latches into SPI the moment it is addressed that way and stays there
    // until the power is cut, so SPI first would spend the one chance at four
    // bits. A failed SDIO attempt never speaks SPI and leaves the card able to
    // answer either way, so falling back costs nothing.
    if (!card_bring_up(true)) card_bring_up(false);
    run_startup_script();

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
