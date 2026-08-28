#include "../../common/myrtos_abi.h"

// mdir -- listar modulkatalogen. OS-9 hade samma verktyg med samma namn, och
// av samma skäl: modulerna är systemets verkliga innehållsförteckning.
//
// Varje rad byggs färdig innan den skickas. En skrivning är odelbar, men en
// rad som består av flera skrivningar hinner brytas av andra processer.
void module_main(void) {
    int32_t t = myrtos_console();
    if (t < 0) { myrtos_exit(); return; }

    myrtos_line_t line;
    myrtos_line_reset(&line);
    myrtos_line_str(&line, "\nModule directory:\n  name          links\n");
    myrtos_line_flush(t, &line);

    char name[12];
    for (uint32_t i = 0; ; i++) {
        int32_t links = myrtos_moddir_get(i, name);
        if (links < 0) break;
        myrtos_line_reset(&line);
        myrtos_line_str(&line, "  ");
        myrtos_line_chars(&line, name, 11);
        myrtos_line_str(&line, "   ");
        myrtos_line_u32(&line, (uint32_t)links);
        myrtos_line_str(&line, "\n");
        myrtos_line_flush(t, &line);
    }
    // Vägen stängs inte: den ärvdes och tillhör den som startade oss.
}
