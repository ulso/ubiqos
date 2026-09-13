#include <stdbool.h>
#include "../../common/myrtos_stdio.h"

// backlight -- how bright the panel is, 0 to 100.
//
// Exists because full brightness is not a neutral default. A 4.3 inch white
// screen at a hundred per cent lights a room in the evening, and for some eyes
// it is not a matter of comfort at all: cone dystrophy makes bright light
// disabling, and a display that cannot be looked at shows nothing.
//
// So the setting is asked for at the keyboard rather than compiled in, and the
// board starts at MYRTOS_BACKLIGHT_DEFAULT instead of at full.

MYRTOS_LIBC_DEFINE

void module_main(int argc, char **argv)
{
    if (myrtos_help(argc, argv,
            "usage: backlight <0-100>\n\n"
            "Sets the panel's brightness. 0 is dark, 100 is as bright as it\n"
            "goes. There is no way to ask what it is: the panel does not say,\n"
            "so this only tells it.\n"))
        return;

    if (argc < 2) {
        myrtos_write_str(MYRTOS_STDOUT, "usage: backlight <0-100>\n");
        return;
    }

    uint32_t percent = 0;
    for (const char *p = argv[1]; *p >= '0' && *p <= '9'; p++)
        percent = percent * 10 + (uint32_t)(*p - '0');

    const int32_t got = myrtos_video_brightness(percent);
    if (got < 0) {
        myrtos_write_str(MYRTOS_STDOUT, "backlight: this board has no panel to dim\n");
        return;
    }
    printf("backlight: %ld %%\n", (long)got);
}
