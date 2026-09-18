#include <ubiqos_abi.h>

// The smallest application that proves the SDK: a module built in another
// repository, landing in the application region of flash, run by name from the
// shell like any other.
//
// The include is angle-bracketed and names no path. That is the SDK's include
// directory, and it is the difference between a module that can live anywhere
// and one that has to sit two directories below common/.

void module_main(int argc, char **argv)
{
    if (ubiqos_help(argc, argv,
            "usage: hello\n\n"
            "Says where it was built from. The example application in the\n"
            "UbiqOS SDK; see sdk/example in the UbiqOS tree.\n")) return;

    ubiqos_write_str(UBIQOS_STDOUT,
                     "hello: built out of tree, loaded from the application "
                     "region\n");
}
