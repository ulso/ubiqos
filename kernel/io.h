#ifndef MYRTOS_IO_H
#define MYRTOS_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"

// The I/O manager, in the spirit of OS-9: processes open a path to a named
// device and write to the path number. The driver owns the hardware.
//
// Devices come from DESCRIPTORS, not from a table in the code. A descriptor is
// a data module on the card stating what the device is called, which driver
// handles it, and carrying a configuration only the driver interprets. Adding a
// device is then a matter of adding a file.

#define MYRTOS_MAX_DRIVERS 8
#define MYRTOS_MAX_DEVICES 8
#define MYRTOS_MAX_PATHS   8
#define MYRTOS_PATH_NONE   (-1)

typedef struct {
    const char *module_name;    // what the descriptor refers to: "UART    MOD"
    int32_t (*configure)(const void *config, uint32_t size);
    int32_t (*open)(void);
    int32_t (*write)(const uint8_t *buf, uint32_t len);
    int32_t (*read)(uint8_t *buf, uint32_t len);   // 0 = nothing right now
    // Whether a read would return anything. A driver without this is never
    // waited on: reads from it keep returning 0, as they did before blocking
    // existed. That is what keeps the send-only UART from parking a shell
    // forever on input that cannot arrive.
    int32_t (*readable)(void);
    // Room to write. Absent means always writable, which is right for a driver
    // that cannot fill up -- the UART writes a byte at a time and blocks in
    // hardware, so waiting on it would never end.
    int32_t (*writable)(void);
    int32_t (*close)(void);
    // Whether a read of nothing means "never" rather than "not yet". A device
    // without this can only fall quiet, and a reader waits; /dev/null has an
    // end, and a reader that waited for it would wait for ever.
    int32_t (*at_eof)(void);
} myrtos_driver_t;

void    myrtos_io_init(void);
uint32_t myrtos_io_device_count(void);

// The nth device's name, twelve bytes out. False when index is past the end.
bool     myrtos_io_device_nth(uint32_t index, char *name_out);
bool myrtos_io_has_device(const char *name);

// Register a device from a descriptor. The driver is looked up by name and is
// handed the configuration tail; an unrecognised name rejects the descriptor.
bool    myrtos_io_add_descriptor(const myrtos_descriptor_t *desc);

int32_t myrtos_io_open(const char *name, int32_t owner_pid);
int32_t myrtos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t myrtos_io_close(int32_t path, int32_t owner_pid);
void    myrtos_io_close_all(int32_t owner_pid);
void    myrtos_io_inherit(int32_t parent_pid, int32_t child_pid);

// Whether a read on this path would return something. False also for a device
// whose driver cannot answer, so that such a path is never blocked on.
bool     myrtos_io_readable(int32_t path, int32_t owner_pid);
int32_t  myrtos_io_readable_count(int32_t path, int32_t owner_pid);

// Which process the interrupt key on this path's device should end, and the
// delivery of that key. See the note in io.c: it is caught where the byte
// arrives because the process it is meant for is not reading anything.
int32_t  myrtos_io_set_foreground(int32_t path, int32_t pid, int32_t owner_pid);
bool     myrtos_io_interrupt(const char *device_name);

// Whether a write would take anything. True for a driver that cannot say.
bool     myrtos_io_writable(int32_t path, int32_t owner_pid);

// --- OPEN FILES -----------------------------------------------------------
// POSIX's own three levels, scaled down: the per-process table above, a shared
// table of open files here, and the filesystem underneath. The middle one is
// what makes a descriptor more than a name -- it carries the position, and it
// is shared by a parent and its children exactly as a fork's descriptors are.
//
// A path string per open file rather than a filesystem handle. Sixty-four bytes
// twelve times over is under a kilobyte, and it costs the filesystem a walk per
// read that a handle would have saved. That is the wrong trade for a database
// and the right one for a machine whose files are read once from start to end.
// Eight, not twelve: the build's heap check refused twelve, which is exactly
// what it is for. Eight open files across the whole machine is generous for
// eight paths per process and thirty-two processes that mostly hold devices.
#define MYRTOS_MAX_OPEN_FILES 8

// Bind an already-resolved absolute path to a free descriptor. Called by the
// filesystem server, which is the only thing that knows the path is real.
int32_t myrtos_io_open_file(const char *abs_path, int32_t owner_pid);

// True when this descriptor is a file rather than a device. The read and write
// system calls ask before deciding whether the work can be done in the trap.
bool     myrtos_io_is_file(int32_t path, int32_t owner_pid);

// The path and position behind a file descriptor, for the server to act on.
bool     myrtos_io_file_at(int32_t path, int32_t owner_pid,
                           const char **path_out, uint32_t *pos_out);
void     myrtos_io_file_advance(int32_t path, int32_t owner_pid, uint32_t n);
int32_t  myrtos_io_file_seek(int32_t path, int32_t owner_pid,
                             int32_t offset, uint32_t whence);

// --- PIPES ----------------------------------------------------------------
// A buffer with two ends. The reader blocks while it is empty and a writer
// still holds the other end; when the last writer closes, an empty pipe reads
// as end of file rather than blocking for ever, which is the whole difference
// between a pipe and a device that has gone quiet.
//
// The waiting is the same machinery devices use -- block_on_read, the readable
// check, and the wake on the timer tick -- because a pipe is exactly a thing
// that sometimes has bytes and sometimes does not.
#define MYRTOS_MAX_PIPES 4
#define MYRTOS_PIPE_BUF  128

// Two descriptors: fds[0] reads, fds[1] writes. -1 when none can be had.
int32_t myrtos_io_pipe(int32_t fds[2], int32_t owner_pid);

// True when a read would return nothing and nothing can ever arrive: an empty
// pipe whose writers have all gone. The read system call asks before blocking,
// since zero from a device means "not yet" and zero from here means "never".
bool    myrtos_io_at_eof(int32_t path, int32_t owner_pid);

// A second descriptor onto the same thing. new_path -1 takes the lowest free
// one; otherwise that number, closing whatever was there. This is dup and dup2,
// and it is what redirection is made of: a shell puts the file on descriptor 1,
// starts the child, and puts its own back.
int32_t myrtos_io_dup(int32_t path, int32_t new_path, int32_t owner_pid);

// Open on a SPECIFIC path number. The kernel uses it to give the first process
// its 0, 1 and 2; ordinary opens take the first free slot.
int32_t myrtos_io_open_as(const char *name, int32_t owner_pid, int32_t path);

#endif
