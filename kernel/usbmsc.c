// The SD card as a USB disk, so a program can be built on the host and copied
// straight to the card.
//
// The whole point of the exercise, and the whole difficulty, is that only one
// side may have the card at a time. A host that mounts a FAT volume caches its
// directory and free-cluster map; so does this filesystem. Let both write and
// the volume is ruined in seconds, and neither side will notice until it reads
// something back.
//
// So the card is handed over explicitly and taken back explicitly. Until then
// this reports no medium, which is what an empty card reader does and what
// every host already knows how to show. That also means the storage function
// can be declared in the descriptor from the start: the alternative -- adding
// it when it is wanted -- means re-enumerating, and re-enumerating drops the
// console session the command was typed into.
//
// Handing it over is exactly what happens when a card is pulled, which was
// built the same evening for a different reason: the volume leaves the table
// and the driver forgets what bus it was using. Taking it back is a mount.

#include <stdint.h>
#include <stdbool.h>
#include "tusb.h"
#include "sdcard.h"
#include "fat32.h"

void ubiqos_print(const char *s);
void ubiqos_print_u32(uint32_t v);
void ubiqos_print_hex(uint32_t v);

static bool     host_has_it;
static uint32_t total_blocks;

// Counted rather than guessed. The first host mount was slow enough to notice
// and then wedged the board, and the two obvious explanations -- the card being
// slow, and the host reading far more than it needs to -- are told apart by how
// many sectors went past. Reported when the card comes back, not while it is
// being read: printing from inside the USB task is what would make it slow.
static uint32_t blocks_read, blocks_written, read_calls;

// Measuring whether the host says anything useful when it finishes with the
// card. Taking it back while the host still has the volume mounted leaves that
// host holding a mount against a device that has stopped answering -- Finder
// hangs and diskutil hangs with it -- so the question is whether there is a
// signal to refuse on. PREVENT/ALLOW MEDIUM REMOVAL is the candidate: hosts
// send prevent when they mount removable media and allow when they let go.
static uint32_t saw_prevent, saw_allow, saw_sync, saw_other;
static uint8_t  other_ops[8];

bool ubiqos_msc_host_has_card(void) { return host_has_it; }

// Give the card to the host. The caller has already taken the volume out of
// the table; this only starts saying yes to the host's enquiries.
bool ubiqos_msc_hand_over(void) {
    if (host_has_it) return true;
    if (!ubiqos_fat_extent(0, &total_blocks) || !total_blocks) return false;
    host_has_it = true;
    return true;
}

// Take it back, whether the host ejected it or the user asked.
void ubiqos_msc_take_back(void) {
    if (host_has_it) {
        ubiqos_print("USB disk: the host read ");
        ubiqos_print_u32(blocks_read);
        ubiqos_print(" sectors in ");
        ubiqos_print_u32(read_calls);
        ubiqos_print(" calls and wrote ");
        ubiqos_print_u32(blocks_written);
        ubiqos_print("\n");
        ubiqos_print("USB disk: prevent ");
        ubiqos_print_u32(saw_prevent);
        ubiqos_print(", allow ");
        ubiqos_print_u32(saw_allow);
        ubiqos_print(", sync ");
        ubiqos_print_u32(saw_sync);
        ubiqos_print(", other ");
        ubiqos_print_u32(saw_other);
        for (uint32_t i = 0; i < saw_other && i < 8; i++) {
            ubiqos_print(" ");
            ubiqos_print_hex(other_ops[i]);
        }
        ubiqos_print("\n");
    }
    host_has_it = false;
    total_blocks = 0;
    blocks_read = blocks_written = read_calls = 0;
    saw_prevent = saw_allow = saw_sync = saw_other = 0;
}

// --- what TinyUSB asks of a disk -------------------------------------------

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4]) {
    (void)lun;
    const char v[] = "UbiqOS  ";
    const char p[] = "SD card         ";
    const char r[] = "1.0 ";
    for (int i = 0; i < 8;  i++) vendor_id[i]   = (uint8_t)v[i];
    for (int i = 0; i < 16; i++) product_id[i]  = (uint8_t)p[i];
    for (int i = 0; i < 4;  i++) product_rev[i] = (uint8_t)r[i];
}

// The one that decides whether the host sees a disk at all. Saying no with the
// right sense code is what makes it an empty reader rather than a broken one:
// 0x02/0x3A/0x00 is NOT READY, MEDIUM NOT PRESENT, and hosts show that as a
// slot with nothing in it and stop asking every few milliseconds.
bool tud_msc_test_unit_ready_cb(uint8_t lun) {
    (void)lun;
    if (host_has_it) return true;
    tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);
    return false;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size) {
    (void)lun;
    *block_count = host_has_it ? total_blocks : 0;
    *block_size  = 512;
}

// Eject, from the host's own Finder or umount. This is the handover coming
// back, and it is why ejecting properly matters: the card is not remounted
// here until the host says it has finished with it.
bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition,
                           bool start, bool load_eject) {
    (void)lun; (void)power_condition;
    if (load_eject && !start) {
        ubiqos_print("USB disk: the host ejected the card\n");
        ubiqos_msc_take_back();
    }
    return true;
}

// Reads and writes are whole sectors and the driver already speaks in those,
// so there is no translation here beyond the offset within a sector -- which
// TinyUSB uses when a transfer does not start on a boundary.
int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize) {
    (void)lun;
    if (!host_has_it || lba >= total_blocks) return -1;

    read_calls++;
    static uint8_t sector[512] __attribute__((aligned(4)));
    uint32_t done = 0;
    while (done < bufsize) {
        if (!ubiqos_sd_read_block(lba + (offset + done) / 512, sector)) return -1;
        blocks_read++;
        uint32_t at = (offset + done) % 512;
        uint32_t n  = 512 - at;
        if (n > bufsize - done) n = bufsize - done;
        for (uint32_t i = 0; i < n; i++) ((uint8_t*)buffer)[done + i] = sector[at + i];
        done += n;
    }
    return (int32_t)done;
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize) {
    (void)lun;
    if (!host_has_it || lba >= total_blocks) return -1;

    // Whole sectors only. A partial write would have to read the sector, patch
    // it and write it back, and the host has no reason to ask for one: the
    // buffer is a sector and the transfers are sector-aligned. Refusing is
    // better than a read-modify-write nobody has tested.
    if (offset || (bufsize % 512)) return -1;

    for (uint32_t i = 0; i < bufsize / 512; i++) {
        if (!ubiqos_sd_write_block(lba + i, buffer + i * 512)) return -1;
        blocks_written++;
    }
    return (int32_t)bufsize;
}

// Everything else. Returning a negative refuses the command, which is what the
// SCSI commands this does not implement deserve.
int32_t tud_msc_scsi_cb(uint8_t lun, const uint8_t scsi_cmd[16],
                        void *buffer, uint16_t bufsize) {
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
    case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
        // Bit 0 of byte 4: 1 says keep the medium in, 0 says it may go.
        if (scsi_cmd[4] & 1) saw_prevent++; else saw_allow++;
        return 0;                       // nothing here can lock a card in
    case 0x35:                          // SYNCHRONIZE CACHE
        saw_sync++;
        return 0;
    default:
        if (saw_other < 8) other_ops[saw_other] = scsi_cmd[0];
        saw_other++;
        tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
        return -1;
    }
}
