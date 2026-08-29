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

// Look a name up in the root directory. Shared by every read path, so the
// walk exists once rather than once per caller.
static bool find_entry(const char *name_83, uint32_t *cluster_out, uint32_t *size_out) {
    uint32_t dir_cluster = root_cluster;

    while (dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return false;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return false;   // end of the directory
                if (sector[e] == 0xe5) continue;       // deleted entry
                if (sector[e + 11] == 0x0f) continue;  // long-name fragment
                if (name_matches(&sector[e], name_83)) {
                    *cluster_out = ((uint32_t)rd16(&sector[e + 20]) << 16) | rd16(&sector[e + 26]);
                    *size_out = rd32(&sector[e + 28]);
                    return true;
                }
            }
        }
        dir_cluster = fat_next_cluster(dir_cluster);
    }
    return false;
}

int32_t myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len) {
    if (!mounted) return -1;

    uint32_t file_cluster = 0, file_size = 0;
    if (!find_entry(name_83, &file_cluster, &file_size)) return -1;

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
                if (sector[e] == 0xe5) continue;          // deleted entry
                if (sector[e + 11] == 0x0f) continue;     // long-name fragment
                if (sector[e + 11] & 0x18) continue;      // directory or volume label
                // macOS puts AppleDouble files next to every file (._NAME),
                // whose short names also end in MOD. They are marked hidden and
                // should not even be read. The CRC check catches them otherwise,
                // but by then we have read 4 kB off the card for nothing.
                if (sector[e + 11] & 0x02) continue;      // hidden
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

// Read a slice of a file. cat cannot hold a whole file in a 4 kB process, so it
// asks for one piece at a time. The cluster chain is walked from the start on
// every call, which is quadratic over a large file -- acceptable while files are
// small, and the place to put a cursor if that stops being true.
int32_t myrtos_fat_read_at(const char *name_83, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (!mounted) return -1;

    uint32_t cluster = 0, file_size = 0;
    if (!find_entry(name_83, &cluster, &file_size)) return -1;
    if (offset >= file_size) return 0;                  // end of file
    if (len > file_size - offset) len = file_size - offset;

    const uint32_t bytes_per_cluster = sectors_per_cluster * 512;
    for (uint32_t skip = offset / bytes_per_cluster; skip; skip--) {
        cluster = fat_next_cluster(cluster);
        if (cluster >= 0x0ffffff8) return -1;           // chain shorter than the size claims
    }

    uint32_t pos = offset % bytes_per_cluster, written = 0;
    while (cluster < 0x0ffffff8 && written < len) {
        for (uint32_t s = pos / 512; s < sectors_per_cluster && written < len; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(cluster) + s, sector)) return -1;
            for (uint32_t i = pos % 512; i < 512 && written < len; i++) buf[written++] = sector[i];
            pos = 0;
        }
        pos = 0;
        cluster = fat_next_cluster(cluster);
    }
    return (int32_t)written;
}

// Enumerate the root directory. Unlike myrtos_fat_find_nth this filters on
// nothing: ls should show what is on the card, not what the module loader cares
// about. Hidden entries stay out, which is both what ls does without -a and
// what keeps macOS AppleDouble files off the listing.
int32_t myrtos_fat_stat_nth(uint32_t index, char *name_out, uint32_t *size_out) {
    if (!mounted) return -1;

    uint32_t seen = 0;
    uint32_t dir_cluster = root_cluster;

    while (dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!myrtos_sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return -1;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return -1;         // end of the directory
                if (sector[e] == 0xe5) continue;          // deleted entry
                if (sector[e + 11] == 0x0f) continue;     // long-name fragment
                if (sector[e + 11] & 0x08) continue;      // volume label
                if (sector[e + 11] & 0x02) continue;      // hidden
                if (seen++ != index) continue;
                for (int i = 0; i < 11; i++) name_out[i] = (char)sector[e + i];
                name_out[11] = 0;
                *size_out = rd32(&sector[e + 28]);
                return (int32_t)(uint8_t)sector[e + 11];
            }
        }
        dir_cluster = fat_next_cluster(dir_cluster);
    }
    return -1;
}

// Turn a name as a person types it into the raw 8.3 form the directory holds:
// "readme.txt" becomes "README  TXT". Without this every utility would have to
// know how FAT pads names, which is the filesystem's business and not theirs.
bool myrtos_fat_name_to_83(const char *user, char *out_11) {
    for (int i = 0; i < 11; i++) out_11[i] = ' ';
    out_11[11] = 0;

    int i = 0;
    while (*user && *user != '.' && i < 8) {
        char c = *user++;
        out_11[i++] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    while (*user && *user != '.') user++;               // a longer stem is truncated
    if (!*user) return i > 0;

    user++;                                             // past the dot
    for (int j = 0; *user && j < 3; j++) {
        char c = *user++;
        out_11[8 + j] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    return i > 0;
}
