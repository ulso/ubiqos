# Building a module outside this tree

Everything a module needs is in [`sdk/myrtos-sdk.cmake`](../sdk/myrtos-sdk.cmake),
and [`sdk/example`](../sdk/example) is a working application that uses it.

It is **extracted, not copied**. This repository's own `CMakeLists.txt` includes
that file and keeps no second version of any of it, so the two cannot drift: if
what is in the SDK were wrong, every module in this tree would be wrong with it.

## What it gives you

`myrtos_add_module(<name> <source>...)` with the trailing words the in-tree
modules use -- `RT`, `SINGLE`, `NEWLIB`, `LIBRARY`, `DRIVER` -- and everything
that makes a module loadable at an address nobody knew at compile time:

* the code model, which on RISC-V is `-mcmodel=medany`
* `-ffixed-r9` on ARM, because the thread pointer lives in a register the
  compiler would otherwise use
* `--no-relax` on RISC-V, which is not an optimisation setting: relaxation turns
  an ordinary access into a `gp`-relative one, and a module's `gp` is not its own
* the position-independence check, which reads the relocations and refuses what
  the loader cannot fix
* `objcopy`, and `make_module.py` to put the header on

`myrtos_app_image(<name> <module>...)` concatenates modules into an image for
the application region and wraps it as a UF2.

`#include <myrtos_abi.h>` works, with no path: the SDK puts `common/` on the
include path. The modules in this tree reach the same file by a relative path
and are unaffected.

## The two regions

The system's modules are at `MYRTOS_FLASH_MODULE_BASE` and an application's at
`MYRTOS_FLASH_APP_BASE`, seven megabytes apart, both in
[`kernel/flashmod.h`](../kernel/flashmod.h) -- which is where the SDK and the
build read them from, so the address is written down once.

The system is scanned first, so a name in both resolves to the system's. An
application cannot shadow the shell by naming a module `sh`.

That is what lets an application be a separate repository. It does not need this
tree's build output to make its image, and updating it does not mean rebuilding
the system underneath it.

## Shipping

Each build produces `myrtos.uf2`: the kernel and the system's modules in one
file, at two addresses with a gap between them, because a UF2 block carries its
own target address.

    python3 tools/combine_uf2.py product.uf2 myrtos.uf2 app.uf2

makes the one file a product ships as. `app.uf2` on its own updates the
application and touches nothing else. Combining an Arm build with a RISC-V one
is refused.

## Reading the sensors

`modules/hibouair` stays in this tree as the reference reader and the worked
example of one: the BleuIO's AT commands, the JSON the dongle answers in, the
HibouAir beacon's fields, and pages for a touch screen.

An application does not extend it. New sensor models appear carrying beacons a
given reader does not decode yet, and that change belongs with whoever ships
the sensors -- so an application carries its own reader, under its own module
name, and is free to update it without touching anything public. What stays
here is the version known to work against the sensors on the bench, which is
what makes it worth reading.

Two modules of the same name, one in each region, would resolve to the system's
-- so give the application's its own name.

## The trap

Read [writing-modules.md](writing-modules.md) before writing anything with a
table of pointers in it. The compiler writes such tables without being asked --
a switch returning string literals is one -- and while the loader relocates
them now, they are what makes a module private rather than shared.
