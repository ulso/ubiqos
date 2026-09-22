#ifndef UBIQOS_IO_H
#define UBIQOS_IO_H

#include <stdint.h>
#include <stdbool.h>
#include "../common/ubiqos_abi.h"

// The I/O manager, in the spirit of OS-9: processes open a path to a named
// device and write to the path number. The driver owns the hardware.
//
// Devices come from DESCRIPTORS, not from a table in the code. A descriptor is
// a data module on the card stating what the device is called, which driver
// handles it, and carrying a configuration only the driver interprets. Adding a
// device is then a matter of adding a file.

// Sixteen, up from eight on 8 Sep 2026, and eight was reached without a word
// being said. Adding the I2C and I2S drivers made nine of each -- five built
// into io.c and four modules -- and /dev quietly came up one short: `term`
// simply was not there. Both limits now say so when they are hit; see
// ubiqos_io_add_descriptor.
//
// The cost of the extra eight is about two hundred bytes of kernel data, which
// is a bad trade only if the number is never approached, and it just was.
#define UBIQOS_MAX_DRIVERS 16
#define UBIQOS_MAX_DEVICES 16
// Twelve, up from eight on 8 Sep 2026. Twelve bytes an entry across every
// process, so the whole rise costs under a kilobyte -- and three of a
// process's eight were always stdin, stdout and stderr, which left five for
// its own work and a pipeline with redirection on both halves can want more.
#define UBIQOS_MAX_PATHS   12
#define UBIQOS_PATH_NONE   (-1)

// ubiqos_driver_t was here. It is in common/ubiqos_abi.h now, because a driver
// may be a module of its own and a module cannot include a kernel header -- the
// same move ubiqos_fsops_t made when fat32 became a library.

void    ubiqos_io_init(void);

// Who owns which pin. See kernel/pins.c: one table, one owner, and a driver
// that wants a pin says so at the moment it configures itself. Not enforcement
// -- nothing stops a driver writing to a pin it never claimed -- but it lets a
// person asking for a pin be told who has it.
void        ubiqos_pins_init(void);
int32_t     ubiqos_pin_claim(uint32_t pin, const char *who);
int32_t     ubiqos_pin_release(uint32_t pin);
const char *ubiqos_pin_owner(uint32_t pin);
uint32_t ubiqos_io_device_count(void);

// The nth device's name, twelve bytes out. False when index is past the end.
bool     ubiqos_io_device_nth(uint32_t index, char *name_out);
bool ubiqos_io_has_device(const char *name);

// Register a device from a descriptor. The driver is looked up by name and is
// handed the configuration tail; an unrecognised name rejects the descriptor.
bool    ubiqos_io_add_descriptor(const ubiqos_descriptor_t *desc);

int32_t ubiqos_io_open(const char *name, int32_t owner_pid);
int32_t ubiqos_io_write(int32_t path, const uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t ubiqos_io_read(int32_t path, uint8_t *buf, uint32_t len, int32_t owner_pid);
int32_t ubiqos_io_close(int32_t path, int32_t owner_pid);

// Everything about a device that is not its data. A pipe has none, a driver
// that does not implement them answers -1, and so does an unknown code.
int32_t ubiqos_io_getstat(int32_t path, uint32_t code, void *data, uint32_t len,
                          int32_t owner_pid);
int32_t ubiqos_io_setstat(int32_t path, uint32_t code, const void *data, uint32_t len,
                          int32_t owner_pid);
void    ubiqos_io_close_all(int32_t owner_pid);
void    ubiqos_io_inherit(int32_t parent_pid, int32_t child_pid);

// Whether a read on this path would return something. False also for a device
// whose driver cannot answer, so that such a path is never blocked on.
bool     ubiqos_io_readable(int32_t path, int32_t owner_pid);
int32_t  ubiqos_io_readable_count(int32_t path, int32_t owner_pid);

// Which process the interrupt key on this path's device should end, and the
// delivery of that key. See the note in io.c: it is caught where the byte
// arrives because the process it is meant for is not reading anything.
int32_t  ubiqos_io_set_foreground(int32_t path, int32_t pid, int32_t owner_pid);
bool     ubiqos_io_interrupt(const char *device_name);

// Whether a write would take anything. True for a driver that cannot say.
bool     ubiqos_io_writable(int32_t path, int32_t owner_pid);

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
// Sixteen. It was eight, and the note here said the build's heap check had
// REFUSED twelve -- so this is the one number on the list that was not chosen
// but forced, back when moving the drivers out of the kernel had not yet
// happened. Seventy-two bytes an entry.
#define UBIQOS_MAX_OPEN_FILES 16

// Bind an already-resolved absolute path to a free descriptor. Called by the
// filesystem server, which is the only thing that knows the path is real.
int32_t ubiqos_io_open_file(const char *abs_path, int32_t owner_pid);

// True when this descriptor is a file rather than a device. The read and write
// system calls ask before deciding whether the work can be done in the trap.
bool     ubiqos_io_is_file(int32_t path, int32_t owner_pid);

// The path and position behind a file descriptor, for the server to act on.
bool     ubiqos_io_file_at(int32_t path, int32_t owner_pid,
                           const char **path_out, uint32_t *pos_out);
void     ubiqos_io_file_advance(int32_t path, int32_t owner_pid, uint32_t n);
int32_t  ubiqos_io_file_seek(int32_t path, int32_t owner_pid,
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
// Eight, up from four. A hundred and forty-four bytes each, buffer included.
#define UBIQOS_MAX_PIPES 8
#define UBIQOS_PIPE_BUF  128

// Two descriptors: fds[0] reads, fds[1] writes. -1 when none can be had.
int32_t ubiqos_io_pipe(int32_t fds[2], int32_t owner_pid);
int32_t ubiqos_io_pipepair(int32_t fds[2], int32_t owner_pid);

// True when a read would return nothing and nothing can ever arrive: an empty
// pipe whose writers have all gone. The read system call asks before blocking,
// since zero from a device means "not yet" and zero from here means "never".
bool    ubiqos_io_at_eof(int32_t path, int32_t owner_pid);

// A second descriptor onto the same thing. new_path -1 takes the lowest free
// one; otherwise that number, closing whatever was there. This is dup and dup2,
// and it is what redirection is made of: a shell puts the file on descriptor 1,
// starts the child, and puts its own back.
int32_t ubiqos_io_dup(int32_t path, int32_t new_path, int32_t owner_pid);

// Open on a SPECIFIC path number. The kernel uses it to give the first process
// its 0, 1 and 2; ordinary opens take the first free slot.
int32_t ubiqos_io_open_as(const char *name, int32_t owner_pid, int32_t path);

#endif
