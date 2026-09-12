// The kernel's side of the SD card library.
//
// The driver was kernel/sdcard.c plus the vendored SDIO driver under
// third_party: 11.6 kB of a kernel that is copied into SRAM at boot and stays
// there for the life of the machine, for something that matters only once
// somebody touches the card. It is modules/sdlib now, and this keeps the seven
// names the rest of the kernel calls, so fsserver.c and usbmsc.c do not know
// the difference.
//
// It is linked on the first call, which is the mount, exactly as wifilib and
// fat32lib are. A board with no sdlib in flash has no card -- which is not a
// silent failure so much as a machine without a card reader.
//
// One thing here is not like the other two. sdlib does DMA, and a module lives
// in PSRAM, which DMA cannot be trusted to reach; so its init asks the kernel
// for SRAM and the driver's buffers come from there. That is what the mem_alloc
// and dma_safe entries of the kernel table are for.
#include <stdint.h>
#include <stdbool.h>
#include "../common/myrtos_abi.h"
#include "moddir.h"
#include "sdcard.h"

void myrtos_print(const char *s);
extern const myrtos_kernel_api_t myrtos_kernel_api;

// The order sdlib publishes them in. Both lists are the interface and neither
// may be reordered alone.
enum { SD_INIT_LIB = 0, SD_INIT = 1, SD_TRY_SDIO = 2, SD_READ = 3, SD_WRITE = 4,
       SD_IS_SDIO = 5, SD_FAILED = 6, SD_FORGET = 7 };

static const myrtos_lib_table_t *lib;

static bool ensure_linked(void)
{
    if (lib) return true;
    lib = myrtos_lib_link("sdlib", 0);
    if (!lib || lib->count <= SD_FORGET) { lib = 0; return false; }

    bool (*init)(const myrtos_kernel_api_t *) =
        (bool (*)(const myrtos_kernel_api_t *))lib->fn[SD_INIT_LIB];
    if (!init(&myrtos_kernel_api)) { lib = 0; return false; }

    myrtos_print("sd: library linked, running from the module pool\n");
    return true;
}

// Whether there is a driver at all, as opposed to no card. Without this the
// two are indistinguishable in the log, and on a board with no sdlib in its
// module list "no card, or SPI was asked for first" sends somebody looking for
// a card that was never going to be found.
bool myrtos_sd_have_driver(void)
{
    return ensure_linked();
}

bool myrtos_sd_init(void)
{
    if (!ensure_linked()) return false;
    return ((bool (*)(void))lib->fn[SD_INIT])();
}

bool myrtos_sd_try_sdio(void)
{
    if (!ensure_linked()) return false;
    return ((bool (*)(void))lib->fn[SD_TRY_SDIO])();
}

// The three below are asked on paths that run whether or not there is a card --
// a USB mass-storage read, the file server checking whether the card went away.
// They answer for an unlinked library rather than linking it: linking is what a
// mount does, and a read of a card nobody has mounted has nothing to say.
bool myrtos_sd_read_block(uint32_t lba, uint8_t *buf)
{
    if (!lib) return false;
    return ((bool (*)(uint32_t, uint8_t *))lib->fn[SD_READ])(lba, buf);
}

bool myrtos_sd_write_block(uint32_t lba, const uint8_t *buf)
{
    if (!lib) return false;
    return ((bool (*)(uint32_t, const uint8_t *))lib->fn[SD_WRITE])(lba, buf);
}

bool myrtos_sd_is_sdio(void)
{
    if (!lib) return false;
    return ((bool (*)(void))lib->fn[SD_IS_SDIO])();
}

// True once the card has stopped answering. With no library there is no card,
// and "failed" would make the file server unmount a volume it never mounted.
bool myrtos_sd_failed(void)
{
    if (!lib) return false;
    return ((bool (*)(void))lib->fn[SD_FAILED])();
}

void myrtos_sd_forget(void)
{
    if (!lib) return;
    ((void (*)(void))lib->fn[SD_FORGET])();
}
