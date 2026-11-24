// Blixt's assembler: Thumb-2 for the Cortex-M33, in UAL syntax.
//
// Two-pass in spirit and as many passes as it takes in practice: an
// instruction that refers forward is first assumed short, and grows when a
// later pass finds its target out of reach. Sizes only ever grow, so the
// passes settle, and one more pass then writes the bytes and the errors.
//
// It knows nothing of files or screens. The editor hands it lines and gets
// back code, a listing (address and size per line) and the first errors.
#ifndef BLIXT_ASM_H
#define BLIXT_ASM_H

#include <stdint.h>
#include <stdbool.h>

#define ASM_MAX_ERRORS 16

typedef struct {
    int  line;              // 0-based
    char msg[80];
} asm_error_t;

typedef struct {
    // The code, as offsets from its first byte.
    uint8_t  *code;
    uint32_t  size;

    // Words holding an address, which the loader must add its base to. Each
    // is the offset of the word in the code; the word itself holds the
    // target as an offset in the code.
    uint32_t *relocs;
    uint32_t  nrelocs;

    uint32_t  entry;        // offset of main or _start, else 0
    uint32_t  stack;        // .stack, else 0 for the default

    // Per source line: where it begins and how many bytes it made.
    uint32_t *line_addr;
    uint32_t *line_size;
    int       nlines;

    asm_error_t errors[ASM_MAX_ERRORS];
    int       nerrors;      // may exceed ASM_MAX_ERRORS; only the first are kept
    int       passes;
} asm_result_t;

// Line i of the source, not terminated; *len says how long.
typedef const char *(*asm_line_fn)(void *ctx, int i, int *len);

// Returns true when there were no errors. The result owns its arrays; free
// them with asm_free.
bool asm_assemble(asm_line_fn get, void *ctx, int nlines, asm_result_t *r);
void asm_free(asm_result_t *r);

#endif
