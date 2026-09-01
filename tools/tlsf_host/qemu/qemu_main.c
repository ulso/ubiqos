// The 32-bit target. No libc: the output goes straight to virt's 16550 UART,
// which is the whole of what this needs from the machine.
//
// This is the run that matters. On the Mac the block header is 64 bits wide and
// twice the board's size, so a fault in the allocator's size arithmetic could
// hide there. Here the word is the board's own.
#include "../checks.h"

#define UART   ((volatile unsigned char *)0x10000000)
#define FINISH ((volatile unsigned int  *)0x00100000)

void out_str(const char *s) { while (*s) *UART = (unsigned char)*s++; }

void out_u32(unsigned long v)
{
    char b[12];
    int i = 11;
    b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    out_str(&b[i]);
}

void qemu_main(void)
{
    out_str("\ntlsf on qemu-system-riscv32\n");
    int bad = tlsf_checks();
    *FINISH = bad ? 0x3333 : 0x5555;        // qemu's test device: fail or pass
}
