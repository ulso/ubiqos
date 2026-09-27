#ifndef PQCLEAN_RANDOMBYTES_H
#define PQCLEAN_RANDOMBYTES_H
// Not PQClean's: theirs reads the operating system's generator. Here the one
// program that uses this, sshd, supplies it from its own CTR-DRBG, seeded from
// the chip's true random number generator -- see PQCLEAN_randombytes there.
#include <stddef.h>
#include <stdint.h>
#define randombytes PQCLEAN_randombytes
int PQCLEAN_randombytes(uint8_t *output, size_t n);
#endif
