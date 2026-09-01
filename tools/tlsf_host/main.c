// The Mac target: output through stdio, and lldb available.
#include "checks.h"
#include <stdio.h>

void out_str(const char *s) { fputs(s, stdout); }
void out_u32(unsigned long v) { printf("%lu", v); }

int main(void) { out_str("\ntlsf on the host\n"); return tlsf_checks(); }
