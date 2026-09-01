// The tests, and the two things a target must provide to run them.
#pragma once
void out_str(const char *s);
void out_u32(unsigned long v);
int  tlsf_checks(void);          // 0 when everything passed
