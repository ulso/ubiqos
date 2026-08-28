#include <stdint.h>
extern "C" {
#include "../../common/myrtos_abi.h"
}

// Klass utan virtuella funktioner: medlemsfunktioner är vanliga funktioner med
// ett dolt this-argument, och inlinas dessutom här. Ingen vtable, inga
// statiska konstruktorer.
class Terminal {
public:
    explicit Terminal(const char *device) : path_(myrtos_open(device)) {}
    bool ok() const { return path_ >= 0; }
    void write(const char *s) const { myrtos_write_str(path_, s); }
    void close() const { myrtos_close(path_); }
private:
    int32_t path_;
};

// extern "C" krävs: bygget slår upp module_main med nm, och C++ skulle annars
// mangla namnet till _Z11module_mainv.
// Statisk polymorfi med CRTP: basklassen känner den härledda typen genom
// mallparametern, så anropet binds vid kompilering. Ingen vtable, alltså inga
// funktionspekare i statiska data.
template <typename Derived>
struct Writer {
    void emit(const Terminal &t) const {
        static_cast<const Derived*>(this)->emit_impl(t);
    }
};

struct Plain : Writer<Plain> {
    void emit_impl(const Terminal &t) const { t.write("[plain] static dispatch\n"); }
};

struct Loud : Writer<Loud> {
    void emit_impl(const Terminal &t) const { t.write("[LOUD] STATIC DISPATCH\n"); }
};

// Behövs körtidsval ändå: bygg tabellen i processens eget minne i stället för
// som statisk initierare. Adresserna beräknas då vid körning och följer med
// modulen, i stället för att länkaren skriver in dem.
using Emitter = void (*)(const Terminal &);

extern "C" {
// used: funktionerna refereras bara från assembler, som kompilatorn inte ser,
// och skulle annars optimeras bort som oanvända.
__attribute__((used)) static const char *say_first(void)  { return "[reltab 0] shared, in .rodata\n"; }
__attribute__((used)) static const char *say_second(void) { return "[reltab 1] shared, in .rodata\n"; }
}
MYRTOS_RELTAB_BEGIN(emitters);
MYRTOS_RELTAB_ENTRY(emitters, say_first);
MYRTOS_RELTAB_ENTRY(emitters, say_second);
MYRTOS_RELTAB_END();

extern "C" void module_main(void) {
    Terminal term("term");
    if (!term.ok()) { myrtos_exit(); return; }

    term.write("\n[c++] classes, templates and RAII, no vtables\n");

    Plain plain; Loud loud;
    plain.emit(term);
    loud.emit(term);

    // Den delade varianten: tabellen ligger const i .rodata och bär avstånd
    // i stället för adresser, så alla processer använder samma kopia.
    term.write(MYRTOS_RELTAB_CALL(emitters, 1, const char *(*)(void))());

    // Och den stackbyggda, som fungerar men ger varje process en egen kopia.
    Emitter table[2] = {
        [](const Terminal &t) { t.write("[table 0] built at runtime\n"); },
        [](const Terminal &t) { t.write("[table 1] built at runtime\n"); },
    };
    table[term.ok() ? 1 : 0](term);
    for (int i = 0; i < 3; i++) {
        term.write("[c++] still position independent\n");
    }
    term.close();
}
