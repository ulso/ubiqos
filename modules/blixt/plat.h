// What Blixt needs from the machine it runs on: a terminal, files, and a way
// to run what it built. plat_ubiqos.c is the board; plat_posix.c is a Mac or
// Linux terminal, where everything but running the program works, so the
// editor and the assembler can be tried without flashing anything.
#ifndef BLIXT_PLAT_H
#define BLIXT_PLAT_H

#include <stdbool.h>
#include <stdint.h>

#include "asm.h"

void     plat_init(void);
void     plat_done(void);
void     plat_write(const char *s, int n);
// Up to max bytes; waits at most timeout_ms for the first (negative: for
// ever). Returns how many arrived, 0 when none did.
int      plat_read(uint8_t *buf, int max, int timeout_ms);
uint32_t plat_ms(void);
// The screen's own size when no terminal answers ESC [ 18 t.
void     plat_default_size(int *rows, int *cols);

// A whole file, NUL-terminated, or 0. The caller frees it.
char    *plat_load(const char *path, long *size);
bool     plat_save(const char *path, const char *data, long size);

// Build a module from the code and run it with the terminal its own; returns
// when it has ended. msg says how it went, for the screen.
bool     plat_run(const char *name, const char *args, const asm_result_t *r,
                  char *msg, int msglen);

// The current directory, for a file opened without one.
void     plat_cwd(char *buf, int size);

// What this machine calls itself, for the About box.
const char *plat_name(void);

#endif
