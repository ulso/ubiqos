#include "fat32.h"
#include "sdcard.h"

void myrtos_print(const char *s);
void myrtos_print_u32(uint32_t v);

static uint32_t fat_start_lba;      // the first FAT
static uint32_t data_start_lba;     // the first data cluster (cluster 2)
static uint32_t sectors_per_cluster;
static uint32_t root_cluster;
static bool     mounted;

static uint8_t sector[512];

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool myrtos_fat_mount(void) {
    mounted = false;

    if (!myrtos_sd_read_block(0, sector)) {
        myrtos_print("FAT: cannot read sector 0\n");
        return false;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        myrtos_print("FAT: no boot signature\n");
        return false;
    }

    // Sector zero is either a partition table or the volume's own boot
    // sector. The jump instruction at the start tells them apart.
    uint32_t vbr_lba = 0;
    if (!(sector[0] == 0xeb || sector[0] == 0xe9)) {
        // Partition table: the first entry starts at 446, the LBA at +8.
        vbr_lba = rd32(&sector[446 + 8]);
        if (!vbr_lba) { myrtos_print("FAT: no partition found\n"); return false; }
        if (!myrtos_sd_read_block(vbr_lba, sector)) return false;
    }

    uint32_t bytes_per_sector = rd16(&sector[11]);
    sectors_per_cluster       = sector[13];
    uint32_t reserved         = rd16(&sector[14]);
    uint32_t num_fats         = sector[16];
    uint32_t sectors_per_fat  = rd32(&sector[36]);
    root_cluster              = rd32(&sector[44]);

    if (bytes_per_sector != 512 || !sectors_per_cluster || !sectors_per_fat) {
        myrtos_print("FAT: not a FAT32 volume this reader understands\n");
        return false;
    }

    fat_start_lba  = vbr_lba + reserved;
    data_start_lba = fat_start_lba + num_fats * sectors_per_fat;
    mounted = true;

    myrtos_print("FAT32 mounted, ");
    myrtos_print_u32(sectors_per_cluster);
    myrtos_print(" sectors per cluster\n");
    return true;
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

// The next cluster in the chain, or >= 0x0ffffff8 when the file ends.
static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t offset = cluster * 4;
    if (!myrtos_sd_read_block(fat_start_lba + offset / 512, sector)) return 0x0fffffff;
    return rd32(&sector[offset % 512]) & 0x0fffffff;
}

// Compare a directory name, which is stored as eleven characters with no dot,
// right-padded with spaces: "SH      MOD".
static bool name_matches(const uint8_t *entry, const char *name_83) {
    for (int i = 0; i < 11; i++) {
        if (entry[i] != (uint8_t)name_83[i]) return false;
    }
    return true;
}

int32_t myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len) {
    if (!mounted) return -1;

    uint32_t file_cluster = 0, file_size = 0;
    uint32_t dir_cluster = root_cluster;

    while (dir_cluster < 0x0ffffff8 && !file_cluster) {
        for (uint32_t s = 0; s < sectors_per_cluster && !file_cluster; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return -1;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return -1;      // end of the directory
                if (sector[e] == 0xe5) continue;       // raderad post
                if (sector[e + 11] == 0x0f) continue;  // long-name fragment
                if (name_matches(&sector[e], name_83)) {
                    file_cluster = ((uint32_t)rd16(&sector[e + 20]) << 16) | rd16(&sector[e + 26]);
                    file_size = rd32(&sector[e + 28]);
                    break;
                }
            }
        }
        if (!file_cluster) dir_cluster = fat_next_cluster(dir_cluster);
    }

    if (!file_cluster) return -1;
    if (file_size > max_len) return -2;

    uint32_t written = 0, cluster = file_cluster;
    while (cluster < 0x0ffffff8 && written < file_size) {
        for (uint32_t s = 0; s < sectors_per_cluster && written < file_size; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(cluster) + s, sector)) return -1;
            for (uint32_t i = 0; i < 512 && written < file_size; i++) {
                buf[written++] = sector[i];
            }
        }
        cluster = fat_next_cluster(cluster);
    }
    return (int32_t)written;
}

bool myrtos_fat_find_nth(const char *ext_3, uint32_t index, char *name_out) {
    if (!mounted) return false;

    uint32_t seen = 0;
    uint32_t dir_cluster = root_cluster;

    while (dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return false;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return false;      // end of the directory
                if (sector[e] == 0xe5) continue;          // raderad
                if (sector[e + 11] == 0x0f) continue;     // long-name fragment
                if (sector[e + 11] & 0x18) continue;      // katalog eller volymnamn
                // macOS puts AppleDouble files next to every file (._NAME),
                // whose short names also end in MOD. They are marked hidden and
                // should not even be read. The CRC check catches them otherwise,
                // but by then we have read 4 kB off the card for nothing.
                if (sector[e + 11] & 0x02) continue;      // dold
                if (sector[e + 8] != (uint8_t)ext_3[0] ||
                    sector[e + 9] != (uint8_t)ext_3[1] ||
                    sector[e + 10] != (uint8_t)ext_3[2]) continue;
                if (seen++ != index) continue;
                for (int i = 0; i < 11; i++) name_out[i] = (char)sector[e + i];
                name_out[11] = 0;
                return true;
            }
        }
        dir_cluster = fat_next_cluster(dir_cluster);
    }
    return false;
}
