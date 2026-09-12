#include <myrtos_abi.h>

// The smallest application that proves the SDK: a module built in another
// repository, landing in the application region of flash, run by name from the
// shell like any other.
//
// The include is angle-bracketed and names no path. That is the SDK's include
// directory, and it is the difference between a module that can live anywhere
// and one that has to sit two directories below common/.

void module_main(int argc, char **argv)
{
    if (myrtos_help(argc, argv,
            "usage: airview\n\n"
            "Says where it was built from. The example application in the\n"
            "myrtos SDK; see sdk/example in the myrtos tree.\n")) return;

    myrtos_write_str(MYRTOS_STDOUT,
                     "airview: built out of tree, loaded from the application "
                     "region\n");
}
