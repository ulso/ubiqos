// FAT32 as a library module: code the kernel calls rather than runs.
//
// This was kernel/fat32.c until 6 Sep 2026 -- 9.3 kB of SRAM that only matters
// once somebody touches the card. It is loaded where modules are loaded, which
// is PSRAM, and the kernel image no longer carries it.
//
// No kernel header, no SDK header. Everything it needs from outside is in K:
// two prints and four block-device calls, which is the whole of what a
// filesystem asked of the system underneath it. myrtos_fsops_t came along to
// the ABI, because it stopped being a kernel detail the moment this file left.
#include "../../common/myrtos_abi.h"
#include "../../common/myrtos_string.h"   // memset and strlen, which the
                                          // compiler emits calls to by itself

static const myrtos_kernel_api_t *K;

// What fat32.h used to declare. The header stayed in the kernel because the
// kernel's own callers still want those four names; everything else was only
// ever this file talking to itself, and now says so here.
bool     myrtos_fat_mount(void);
bool     myrtos_fat_remount(void);
bool     myrtos_fat_extent(uint32_t *first_block, uint32_t *block_count);
int32_t  myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len);
bool     myrtos_fat_find_nth(const char *ext_3, uint32_t index, char *name_out);
int32_t  myrtos_fat_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len);
int32_t  myrtos_fat_stat_nth(const char *dirpath, uint32_t index,
                             char *name_out, uint32_t *size_out);
int32_t  myrtos_fat_write_at(const char *path, uint32_t offset,
                             const uint8_t *buf, uint32_t len);
bool     myrtos_fat_remove(const char *path);
bool     myrtos_fat_rename(const char *from, const char *to);
bool     myrtos_fat_mkdir(const char *path);
bool     myrtos_fat_rmdir(const char *path);
int32_t  myrtos_fat_stat(const char *path, uint32_t *size_out);
bool     myrtos_fat_name_to_83(const char *user, char *out_11);
extern const myrtos_fsops_t myrtos_fat_ops;


static uint32_t fat_start_lba;      // the first FAT
static uint32_t data_start_lba;     // the first data cluster (cluster 2)
static uint32_t sectors_per_cluster;
static uint32_t root_cluster;
static uint32_t num_fats;           // every copy must be kept in step
static uint32_t sectors_per_fat;
static uint32_t cluster_count;      // bounds the search for a free cluster

// Where the volume sits on the card and how long it is. Kept because the mass
// storage device needs a size to report, and this is the only place anything
// here reads one. It is the end of the FAT volume, not the end of the card --
// there may be unpartitioned space past it that nothing has looked at.
static uint32_t volume_lba;
static uint32_t volume_sectors;
static bool     mounted;

static uint8_t sector[512] __attribute__((aligned(4)));

// The FAT gets a buffer of its own. Allocation walks the FAT while a directory
// entry or a data sector is being held in `sector`, and one shared buffer would
// have them overwrite each other.
static uint8_t fatbuf[512] __attribute__((aligned(4)));

// Which FAT sector fatbuf holds, if any. fat_get read a sector from the card for
// every entry, and entries come 128 to a sector, so following a chain or
// looking for a free cluster read the same sector again and again: a 385 kB
// file took four minutes to write, and the next file longer than that. Every
// write to the FAT goes through fat_put, which writes fatbuf itself, so the
// copy stays true. A mount forgets it -- the host may have written the card.
static uint32_t fatbuf_lba = UINT32_MAX;

// Where the search for a free cluster starts: just after the last one found.
// It used to start at cluster 2 every time, which made giving a file its
// clusters cost the square of its length.
static uint32_t alloc_hint = 2;

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

void myrtos_fat_forget_read_cache(void);   // defined with the cache, below

bool myrtos_fat_mount(void) {
    fatbuf_lba = UINT32_MAX;
    alloc_hint = 2;
    myrtos_fat_forget_read_cache();
    mounted = false;

    if (!K->sd_read_block(0, sector)) {
        K->print("FAT: cannot read sector 0\n");
        return false;
    }
    if (sector[510] != 0x55 || sector[511] != 0xaa) {
        K->print("FAT: no boot signature\n");
        return false;
    }

    // Sector zero is either a partition table or the volume's own boot
    // sector. The jump instruction at the start tells them apart.
    uint32_t vbr_lba = 0;
    volume_lba = volume_sectors = 0;
    if (!(sector[0] == 0xeb || sector[0] == 0xe9)) {
        // Partition table: the first entry starts at 446, the LBA at +8.
        vbr_lba = rd32(&sector[446 + 8]);
        if (!vbr_lba) { K->print("FAT: no partition found\n"); return false; }
        if (!K->sd_read_block(vbr_lba, sector)) return false;
    }

    uint32_t bytes_per_sector = rd16(&sector[11]);
    sectors_per_cluster       = sector[13];
    uint32_t reserved         = rd16(&sector[14]);
    uint32_t total_sectors    = rd32(&sector[32]);
    num_fats                  = sector[16];
    sectors_per_fat           = rd32(&sector[36]);
    root_cluster              = rd32(&sector[44]);

    // FAT16 and FAT12 say so in two fields FAT32 leaves at zero: a root
    // directory of a fixed number of entries, and a 16-bit FAT size. Naming
    // them is worth the four lines, because the alternative is what happened on
    // a 128 MB card: offsets 36 and 44 held a drive number and half a volume
    // label, the numbers read as nonsense, and `ls` reported an empty card that
    // had a file on it. A reader that cannot read something should say which
    // something.
    const uint32_t root_entries   = rd16(&sector[17]);
    const uint32_t fat_size_16    = rd16(&sector[22]);
    if (root_entries || fat_size_16) {
        K->print("FAT: this is FAT16 or FAT12, and this reader is FAT32 only.\n"
                 "     Reformat the card as FAT32. macOS formats a small card\n"
                 "     as FAT16 whatever it is asked; Windows does as it is told.\n");
        return false;
    }

    if (bytes_per_sector != 512 || !sectors_per_cluster || !sectors_per_fat) {
        K->print("FAT: not a FAT32 volume this reader understands\n");
        return false;
    }

    volume_lba     = vbr_lba;
    volume_sectors = total_sectors;
    fat_start_lba  = vbr_lba + reserved;
    data_start_lba = fat_start_lba + num_fats * sectors_per_fat;
    cluster_count  = (total_sectors - (reserved + num_fats * sectors_per_fat))
                     / sectors_per_cluster;
    mounted = true;

    K->print("FAT32 mounted, ");
    K->print_u32(sectors_per_cluster);
    K->print(" sectors per cluster\n");
    return true;
}

static uint32_t cluster_to_lba(uint32_t cluster) {
    return data_start_lba + (cluster - 2) * sectors_per_cluster;
}

// The next cluster in the chain, or >= 0x0ffffff8 when the file ends.
static uint32_t fat_next_cluster(uint32_t cluster) {
    uint32_t offset = cluster * 4;
    if (!K->sd_read_block(fat_start_lba + offset / 512, sector)) return 0x0fffffff;
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

// --- LONG NAMES -----------------------------------------------------------
// 8.3 is not a format anyone wants to ship applications in. ".wasm" does not
// fit a three-character extension at all, so a file copied from a Mac arrives
// as HELL~36.WAS -- it runs, but nobody would call that a filename.
//
// VFAT keeps the real name in extra directory entries placed BEFORE the short
// one, each marked with attribute 0x0f and holding thirteen UTF-16 units at
// three separate offsets, because the fields had to fit around a layout that
// was already fixed. They are stored last fragment first, and byte 0 carries
// the sequence number, with 0x40 set on the one that ends the name.
//
// This half reads them. The writing half creates and erases them, and the
// machinery for that is with the other write operations further down: fragments
// have to be laid out and torn down in step with the short entry they belong to,
// and a mistake there costs the volume rather than one file.
//
// Sixty-four characters. OS-9 allowed twenty-nine and nobody found it short.
#define FAT_LFN_MAX 64

typedef struct {
    char name[FAT_LFN_MAX + 1];
    bool valid;
} lfn_t;

static void lfn_reset(lfn_t *l) { l->valid = false; l->name[0] = 0; }

static void lfn_take(lfn_t *l, const uint8_t *e) {
    static const uint8_t off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    uint32_t seq = e[0] & 0x3fu;
    if (!seq || (seq - 1) * 13u >= FAT_LFN_MAX) { lfn_reset(l); return; }

    uint32_t base = (seq - 1) * 13u;
    for (uint32_t i = 0; i < 13 && base + i < FAT_LFN_MAX; i++) {
        uint16_t u = rd16(&e[off[i]]);
        // Anything outside ASCII becomes a question mark rather than a truncated
        // byte: a name that is visibly wrong is better than one that is silently
        // half right and matches something it should not.
        l->name[base + i] = (u == 0 || u == 0xffff) ? 0
                          : (u < 128 ? (char)u : '?');
    }
    if (e[0] & 0x40u) {
        uint32_t end = base + 13u;
        if (end > FAT_LFN_MAX) end = FAT_LFN_MAX;
        l->name[end] = 0;                 // for a name that exactly fills the last
    }
    l->valid = true;
}

// Long names are matched without regard to case, as every system that has them
// does. The short name beside them stays upper case and exactly matched, so both
// spellings of a file keep working.
static bool same_name_ci(const char *a, const char *b) {
    while (*a && *b) {
        char x = (*a >= 'A' && *a <= 'Z') ? (char)(*a + 32) : *a;
        char y = (*b >= 'A' && *b <= 'Z') ? (char)(*b + 32) : *b;
        if (x != y) return false;
        a++; b++;
    }
    return *a == *b;
}

// Look a name up in one directory. Shared by every read path, so the walk
// exists once rather than once per caller. The directory is a parameter now:
// the root is just the one whose cluster the boot sector names.
static bool find_entry(uint32_t dir_cluster, const char *name_83,
                       const char *want_long,
                       uint32_t *cluster_out, uint32_t *size_out, uint8_t *attr_out) {
    lfn_t lfn;
    lfn_reset(&lfn);

    while (dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!K->sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return false;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return false;   // end of the directory
                if (sector[e] == 0xe5) { lfn_reset(&lfn); continue; }   // deleted
                if (sector[e + 11] == 0x0f) { lfn_take(&lfn, &sector[e]); continue; }

                // The fragments belong to the short entry that follows them, so
                // the match is made here and the accumulator cleared either way.
                bool hit = name_matches(&sector[e], name_83)
                        || (lfn.valid && want_long && same_name_ci(lfn.name, want_long));
                lfn_reset(&lfn);
                if (hit) {
                    *cluster_out = ((uint32_t)rd16(&sector[e + 20]) << 16) | rd16(&sector[e + 26]);
                    *size_out = rd32(&sector[e + 28]);
                    if (attr_out) *attr_out = sector[e + 11];
                    return true;
                }
            }
        }
        dir_cluster = fat_next_cluster(dir_cluster);
    }
    return false;
}

// --- PATHS ----------------------------------------------------------------
// A path is resolved one component at a time, each lookup starting where the
// last one ended. "." stays put and ".." follows the entry FAT keeps for it --
// which holds zero in a directory whose parent is the root, since the root has
// no cluster number of its own in the eyes of a subdirectory.

static const char *skip_slashes(const char *p) {
    while (*p == '/') p++;
    return p;
}

// Copy one component out of the path and convert it to 8.3 form. Returns where
// the path continues, or 0 when the component is unusable.
//
// "." and ".." are spelled out here rather than left to the name converter,
// which looks for a stem before the dot, finds none, and calls the whole thing
// unusable. FAT stores them as a dot and ten spaces, and two dots and nine.
static const char *component_83(const char *p, char *out_11, char *raw_out) {
    char part[FAT_LFN_MAX + 1];
    uint32_t n = 0;
    while (*p && *p != '/' && n < sizeof(part) - 1) part[n++] = *p++;
    part[n] = 0;
    if (raw_out) {
        uint32_t i = 0;
        for (; part[i]; i++) raw_out[i] = part[i];
        raw_out[i] = 0;
    }
    while (*p && *p != '/') p++;              // a longer component is truncated

    if (part[0] == '.' && (part[1] == 0 || (part[1] == '.' && part[2] == 0))) {
        for (int i = 0; i < 11; i++) out_11[i] = ' ';
        out_11[0] = '.';
        if (part[1] == '.') out_11[1] = '.';
        out_11[11] = 0;
        return p;
    }
    // A component with no 8.3 form is not an error: it is a long name, and
    // raw_out carries it for find_entry to match. out_11 stays blank, which
    // matches nothing, so the two spellings cannot be confused.
    if (!myrtos_fat_name_to_83(part, out_11) && !(raw_out && raw_out[0])) return 0;
    return p;
}

// Resolve a path that names a directory. An empty path, or one that is only
// slashes, is the root.
static bool resolve_dir(const char *path, uint32_t *dir_out) {
    uint32_t dir = root_cluster;
    const char *p = skip_slashes(path ? path : "");
    while (*p) {
        char name[12], raw[FAT_LFN_MAX + 1];
        const char *next = component_83(p, name, raw);
        if (!next) return false;
        if (name[0] == '.' && name[1] == ' ') {
            /* "." is where we already are */
        } else if (name[0] == '.' && name[1] == '.' && name[2] == ' ') {
            // The root has no ".." entry of its own, so failing to find one
            // means we are already at the top and stay there.
            uint32_t cl = 0, sz = 0; uint8_t attr = 0;
            if (find_entry(dir, name, 0, &cl, &sz, &attr))
                dir = cl ? cl : root_cluster;            // FAT writes zero for the root
        } else {
            uint32_t cl = 0, sz = 0; uint8_t attr = 0;
            if (!find_entry(dir, name, raw, &cl, &sz, &attr)) return false;
            if (!(attr & 0x10)) return false;            // a file cannot be walked through
            dir = cl;
        }
        p = skip_slashes(next);
    }
    *dir_out = dir;
    return true;
}

// Resolve everything but the last component, which is handed back in 8.3 form.
// "/docs/readme.txt" gives the cluster of docs and "README  TXT".
static bool resolve_parent(const char *path, uint32_t *dir_out, char *leaf_83,
                           char *leaf_long) {
    const char *p = path ? path : "";
    const char *last = p, *scan = p;
    while (*scan) { if (*scan == '/') last = scan + 1; scan++; }

    char parent[80];
    uint32_t n = 0;
    while (p + n < last && n < sizeof(parent) - 1) { parent[n] = p[n]; n++; }
    parent[n] = 0;

    if (!resolve_dir(parent, dir_out)) return false;
    if (leaf_long) {
        uint32_t i = 0;
        while (last[i] && i < FAT_LFN_MAX) { leaf_long[i] = last[i]; i++; }
        leaf_long[i] = 0;
    }
    // A leaf that has no 8.3 form at all -- ".wasm" has none -- is still a
    // perfectly good long name, so this is not the end of the lookup.
    return myrtos_fat_name_to_83(last, leaf_83) || (leaf_long && leaf_long[0]);
}

int32_t myrtos_fat_read_file(const char *name_83, uint8_t *buf, uint32_t max_len) {
    if (!mounted) return -1;

    uint32_t file_cluster = 0, file_size = 0;
    if (!find_entry(root_cluster, name_83, 0, &file_cluster, &file_size, 0)) return -1;

    if (!file_cluster) return -1;
    if (file_size > max_len) return -2;

    uint32_t written = 0, cluster = file_cluster;
    while (cluster < 0x0ffffff8 && written < file_size) {
        for (uint32_t s = 0; s < sectors_per_cluster && written < file_size; s++) {
            if (!K->sd_read_block(cluster_to_lba(cluster) + s, sector)) return -1;
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
            if (!K->sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return false;
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
// One named entry, which is what read_at already does before it reads -- the
// two helpers were here all along and nothing needed adding to find them.
int32_t myrtos_fat_stat(const char *path, uint32_t *size_out) {
    if (!mounted) return -1;
    if (size_out) *size_out = 0;

    // The volume's own root has no directory entry to look up. It exists.
    const char *p = path ? path : "";
    if (!p[0] || (p[0] == '/' && !p[1])) return 0x10;

    uint32_t dir = 0; char name_83[12], leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(path, &dir, name_83, leaf)) return -1;

    uint32_t cluster = 0, file_size = 0; uint8_t attr = 0;
    if (!find_entry(dir, name_83, leaf, &cluster, &file_size, &attr)) return -1;
    if (size_out) *size_out = file_size;
    return (int32_t)attr;
}

// Reading a file used to cost more than reading the card. Every read_at call
// resolved the path from the root, walked the directory to find the entry, and
// then spooled the cluster chain from the file's first cluster to the offset it
// wanted -- and fat_next_cluster is an SD block read per link. cat asks in
// small pieces, so a 100 kB file took 391 calls, and call k spooled k/16
// clusters: about 4800 block reads of FAT to deliver 100 kB of data. Measured
// at 49 kB/s over four-bit SDIO, on a bus doing 1.5 MB/s underneath. The card
// was never the problem.
//
// So remember where the last read left off. One file, because reading is
// overwhelmingly sequential and a second file simply takes the slot.
static char     ra_path[64];       // as long as the open-file table's own
static bool     ra_valid;
static uint32_t ra_first, ra_size;
static uint32_t ra_index;                 // which link of the chain ra_cluster is
static uint32_t ra_cluster;

// Anything that moves data or entries around drops it. This is deliberately
// blunt: a stale cluster number reads the wrong sector and hands back another
// file's bytes, which is far worse than the walk it saves.
void myrtos_fat_forget_read_cache(void) { ra_valid = false; }

// How much of the card is worth exposing: everything up to the end of the
// mounted volume, counted from block zero so the partition table comes with it.
// A host then sees what a card reader would show it and mounts it the same way.
bool myrtos_fat_extent(uint32_t *first_block, uint32_t *block_count) {
    if (!mounted || !volume_sectors) return false;
    if (first_block) *first_block = volume_lba;
    if (block_count) *block_count = volume_lba + volume_sectors;
    return true;
}

static bool same_path(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

int32_t myrtos_fat_read_at(const char *path, uint32_t offset, uint8_t *buf, uint32_t len) {
    if (!mounted) return -1;

    if (!ra_valid || !same_path(path, ra_path)) {
        uint32_t dir = 0; char name_83[12], leaf[FAT_LFN_MAX + 1];
        if (!resolve_parent(path, &dir, name_83, leaf)) return -1;

        uint32_t cluster = 0, file_size = 0; uint8_t attr = 0;
        if (!find_entry(dir, name_83, leaf, &cluster, &file_size, &attr)) return -1;
        if (attr & 0x10) return -1;                      // a directory is not readable

        uint32_t n = 0;
        while (path[n] && n < sizeof(ra_path) - 1) { ra_path[n] = path[n]; n++; }
        ra_path[n] = 0;
        ra_first = cluster; ra_size = file_size;
        ra_index = 0; ra_cluster = cluster;
        ra_valid = true;
    }

    uint32_t file_size = ra_size;
    if (offset >= file_size) return 0;                  // end of file
    if (len > file_size - offset) len = file_size - offset;

    // Forward from where the last read stopped, and only backwards from the
    // start -- which is what a seek to somewhere earlier costs, and no worse
    // than every call used to cost.
    const uint32_t bytes_per_cluster = sectors_per_cluster * 512;
    uint32_t want = offset / bytes_per_cluster;
    if (want < ra_index) { ra_index = 0; ra_cluster = ra_first; }
    while (ra_index < want) {
        ra_cluster = fat_next_cluster(ra_cluster);
        if (ra_cluster >= 0x0ffffff8) { ra_valid = false; return -1; }
        ra_index++;
    }
    uint32_t cluster = ra_cluster;

    uint32_t pos = offset % bytes_per_cluster, written = 0;
    while (cluster < 0x0ffffff8 && written < len) {
        for (uint32_t s = pos / 512; s < sectors_per_cluster && written < len; s++) {
            if (!K->sd_read_block(cluster_to_lba(cluster) + s, sector)) return -1;
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
int32_t myrtos_fat_stat_nth(const char *dirpath, uint32_t index,
                            char *name_out, uint32_t *size_out) {
    if (!mounted) return -1;

    uint32_t seen = 0, dir_cluster = 0;
    if (!resolve_dir(dirpath, &dir_cluster)) return -1;

    lfn_t lfn;
    lfn_reset(&lfn);

    while (dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!K->sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return -1;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) return -1;         // end of the directory
                if (sector[e] == 0xe5) { lfn_reset(&lfn); continue; }
                if (sector[e + 11] == 0x0f) { lfn_take(&lfn, &sector[e]); continue; }
                if (sector[e + 11] & 0x08) { lfn_reset(&lfn); continue; }   // volume label
                if (sector[e + 11] & 0x02) { lfn_reset(&lfn); continue; }   // hidden
                bool have_long = lfn.valid && lfn.name[0];
                char kept[FAT_LFN_MAX + 1];
                if (have_long) { uint32_t i = 0; for (; lfn.name[i]; i++) kept[i] = lfn.name[i]; kept[i] = 0; }
                lfn_reset(&lfn);
                if (seen++ != index) continue;

                // The long name if the file has one, and the 8.3 name if not --
                // callers get a name they can hand straight back to open, which
                // is the only kind worth returning.
                if (have_long) {
                    uint32_t i = 0;
                    for (; kept[i] && i < MYRTOS_DIRNAME_MAX - 1; i++) name_out[i] = kept[i];
                    name_out[i] = 0;
                } else {
                    for (int i = 0; i < 11; i++) name_out[i] = (char)sector[e + i];
                    name_out[11] = 0;
                }
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
// False when the name has no 8.3 form at all, and out_11 is then left blank
// rather than half filled.
//
// This used to truncate: a stem past eight characters simply lost its tail, and
// so did an extension past three. That is how "hibouair&" -- a mistyped command,
// with the ampersand meant for the shell -- became "HIBOUAIR" and ran
// hibouair.mod. A typo started the right program for the wrong reason, and the
// only sign of it was the loader saying "Loaded hibouair& from /sd".
//
// A partial name must never match, because a match is an identity. Callers that
// have the name as the user typed it fall back to it -- resolve_parent hands
// back both spellings and find_entry tries each -- and callers that do not, the
// module loader among them, get the refusal they should have had.
bool myrtos_fat_name_to_83(const char *user, char *out_11) {
    for (int i = 0; i < 11; i++) out_11[i] = ' ';
    out_11[11] = 0;

    const char *dot = 0;
    for (const char *q = user; *q; q++) if (*q == '.') dot = q;

    uint32_t stem = dot ? (uint32_t)(dot - user) : 0;
    if (!dot) { const char *q = user; while (*q) q++; stem = (uint32_t)(q - user); }
    uint32_t ext = 0;
    if (dot) { const char *q = dot + 1; while (*q) { ext++; q++; } }

    if (!stem || stem > 8 || ext > 3) return false;

    // One dot only. "a.b.c" has no short form either, and silently taking the
    // last of them would be the same kind of guess.
    for (const char *q = user; *q; q++)
        if (*q == '.' && q != dot) return false;

    for (uint32_t i = 0; i < stem; i++) {
        char c = user[i];
        out_11[i] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    for (uint32_t j = 0; j < ext; j++) {
        char c = dot[1 + j];
        out_11[8 + j] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    return true;
}

// --- WRITING --------------------------------------------------------------
// Everything below can destroy the volume if it is wrong, so it is deliberately
// literal: no cached FAT sectors, no deferred writes, every change on the card
// before the next step begins. Slow, and easy to reason about when a file comes
// back wrong.

static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t fat_get(uint32_t cluster) {
    uint32_t offset = cluster * 4;
    const uint32_t lba = fat_start_lba + offset / 512;
    if (lba != fatbuf_lba) {
        if (!K->sd_read_block(lba, fatbuf)) { fatbuf_lba = UINT32_MAX; return 0x0fffffff; }
        fatbuf_lba = lba;
    }
    return rd32(&fatbuf[offset % 512]) & 0x0fffffff;
}

// Write one FAT entry, in every copy of the FAT. Updating only the first leaves
// the volume inconsistent, and whether that matters depends on which copy the
// next reader trusts -- so it is not a corner to cut.
static bool fat_put(uint32_t cluster, uint32_t value) {
    uint32_t offset = cluster * 4;
    uint32_t lba = fat_start_lba + offset / 512;
    if (lba != fatbuf_lba) {
        if (!K->sd_read_block(lba, fatbuf)) { fatbuf_lba = UINT32_MAX; return false; }
        fatbuf_lba = lba;
    }

    // The top four bits are reserved and must be preserved, not overwritten.
    uint32_t old = rd32(&fatbuf[offset % 512]);
    wr32(&fatbuf[offset % 512], (old & 0xf0000000u) | (value & 0x0fffffffu));

    // A failed write leaves fatbuf saying something the card may not, so it is
    // no longer taken as the card's copy.
    for (uint32_t f = 0; f < num_fats; f++) {
        if (!K->sd_write_block(lba + f * sectors_per_fat, fatbuf)) {
            fatbuf_lba = UINT32_MAX;
            return false;
        }
    }
    return true;
}

// First free cluster, marked as end-of-chain so a second call cannot hand out
// the same one. Returns 0 when the volume is full.
static uint32_t fat_alloc(void) {
    const uint32_t end = cluster_count + 2;
    if (alloc_hint < 2 || alloc_hint >= end) alloc_hint = 2;
    uint32_t c = alloc_hint;
    for (uint32_t n = 0; n < cluster_count; n++) {
        if (fat_get(c) == 0) {
            if (!fat_put(c, 0x0ffffff8)) return 0;
            alloc_hint = c + 1;
            return c;
        }
        c = c + 1 < end ? c + 1 : 2;      // round to the start, for freed ones
    }
    return 0;
}

static void fat_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < 0x0ffffff8) {
        uint32_t next = fat_get(cluster);
        if (!fat_put(cluster, 0)) return;
        cluster = next;
    }
}

// Locate a directory entry and say where on the card it lives, so the size and
// starting cluster can be written back after the data is on disk.
// --- DIRECTORY ENTRIES, COUNTED FROM THE START ----------------------------
// A file with a long name occupies several consecutive entries: the fragments
// of its name and then the short entry they belong to. That run can straddle a
// sector and can straddle a cluster, so nothing here may hold on to an (lba,
// offset) pair and step it forward by hand.
//
// Instead an entry is addressed by its number counted from the start of the
// directory, and dir_at walks to it. Several walks where one would do, and every
// one of them obviously correct -- the trade this half of the file makes
// everywhere, for the reason at the top of it.
//
// fat_get, never fat_next_cluster: the latter reads the FAT into `sector`, which
// is where the directory entry being worked on is sitting.
static bool dir_at(uint32_t dir_cluster, uint32_t index,
                   uint32_t *lba_out, uint32_t *off_out) {
    const uint32_t per_sector = 512 / 32;
    while (dir_cluster >= 2 && dir_cluster < 0x0ffffff8) {
        uint32_t per_cluster = sectors_per_cluster * per_sector;
        if (index < per_cluster) {
            *lba_out = cluster_to_lba(dir_cluster) + index / per_sector;
            *off_out = (index % per_sector) * 32;
            return true;
        }
        index -= per_cluster;
        dir_cluster = fat_get(dir_cluster);
    }
    return false;
}

// Every fragment carries this, computed over the eleven bytes of the short name
// it belongs to. It is what ties a run of fragments to its entry: a system that
// finds the checksum disagreeing knows the long name is stale and falls back to
// the short one. Getting it wrong does not corrupt anything, it just means
// nothing else will ever show the long name.
static uint8_t lfn_checksum(const uint8_t *name_83) {
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + name_83[i]);
    return sum;
}

// Whether a name needs fragments at all.
//
// Lowercase deliberately does not count. A short name is stored upper case and
// this filesystem's listing lowercases it on the way out, so "readme.txt" comes
// back as it went in -- and writing fragments for every file would double the
// size of every directory to preserve something already preserved. What needs
// them is a name FAT genuinely cannot hold: a stem past eight, an extension
// past three, more than one dot, or a character 8.3 has no room for.
static bool needs_lfn(const char *name) {
    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;

    uint32_t stem = 0, ext = 0;
    for (const char *p = name; *p; p++) {
        char c = *p;
        if (c == '.') { if (p != dot) return true; continue; }
        if (c == ' ' || c == '+' || c == ',' || c == ';' ||
            c == '=' || c == '[' || c == ']') return true;
        if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7e) return true;
        if (dot && p > dot) ext++; else stem++;
    }
    return stem == 0 || stem > 8 || ext > 3;
}

static char short_char(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    if (c == '-' || c == '_' || c == '~' || c == '!' || c == '#' ||
        c == '$' || c == '%' || c == '&' || c == '@' || c == '^') return c;
    return '_';
}

// The 8.3 alias, "PROGRA~1" fashion. Every long name must have one, because it
// is what the rest of the volume identifies the file by -- the fragments are
// decoration on top of an ordinary entry.
//
// The tail eats into the stem rather than being added to it. Eight characters
// is eight characters, and a name built past that is a name another system will
// not agree with.
static void short_from_long(const char *name, uint32_t tail, char *out_11) {
    for (int i = 0; i < 11; i++) out_11[i] = ' ';
    out_11[11] = 0;

    const char *dot = 0;
    for (const char *p = name; *p; p++) if (*p == '.') dot = p;

    char digits[6];
    uint32_t nd = 0, t = tail;
    do { digits[nd++] = (char)('0' + t % 10); t /= 10; } while (t && nd < sizeof(digits));

    uint32_t room = (nd + 1 < 8) ? 8 - (nd + 1) : 1;
    uint32_t i = 0;
    for (const char *p = name; *p && i < room; p++) {
        if (dot && p >= dot) break;
        if (*p == ' ' || *p == '.') continue;
        out_11[i++] = short_char(*p);
    }
    out_11[i++] = '~';
    while (nd && i < 8) out_11[i++] = digits[--nd];

    if (dot) {
        uint32_t j = 0;
        for (const char *p = dot + 1; *p && j < 3; p++) out_11[8 + j++] = short_char(*p);
    }
}

// Find an entry by either spelling, and say where its whole run begins.
//
// first_idx is what deletion needs: the first fragment, or the short entry
// itself when there are none. The accumulator is cleared at every short entry
// whether it matched or not, because a fragment left over from one file must
// never be read as part of another's name.
static bool dir_find(uint32_t dir_cluster, const char *name_83, const char *want_long,
                     uint32_t *lba_out, uint32_t *off_out,
                     uint32_t *first_idx, uint32_t *short_idx) {
    lfn_t lfn;
    lfn_reset(&lfn);
    uint32_t index = 0, run_start = 0;
    bool in_run = false;

    while (dir_cluster >= 2 && dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint32_t lba = cluster_to_lba(dir_cluster) + s;
            if (!K->sd_read_block(lba, sector)) return false;
            for (uint32_t e = 0; e < 512; e += 32, index++) {
                if (sector[e] == 0x00) return false;
                if (sector[e] == 0xe5) { lfn_reset(&lfn); in_run = false; continue; }
                if (sector[e + 11] == 0x0f) {
                    if (!in_run) { run_start = index; in_run = true; }
                    lfn_take(&lfn, &sector[e]);
                    continue;
                }
                bool hit = name_matches(&sector[e], name_83)
                        || (lfn.valid && want_long && same_name_ci(lfn.name, want_long));
                if (hit) {
                    // Guarded like the two below, which they always were. A
                    // caller that only wants to know whether a name exists has
                    // no use for the position, and passing null for it should
                    // not be a null write -- rename's existence check did
                    // exactly that and was one taken branch away from a fault.
                    if (lba_out) *lba_out = lba;
                    if (off_out) *off_out = e;
                    if (short_idx) *short_idx = index;
                    if (first_idx) *first_idx = in_run ? run_start : index;
                    return true;                 // `sector` still holds this entry
                }
                lfn_reset(&lfn);
                in_run = false;
            }
        }
        dir_cluster = fat_get(dir_cluster);
    }
    return false;
}

static bool dir_locate(uint32_t dir_cluster, const char *name_83,
                       uint32_t *lba_out, uint32_t *off_out) {
    return dir_find(dir_cluster, name_83, 0, lba_out, off_out, 0, 0);
}

// A short name no other entry in this directory holds. Windows does exactly
// this, and for the same reason: the alias has to be unique or two files answer
// to one name.
static bool unique_short(uint32_t dir_cluster, const char *name, char *out_11) {
    for (uint32_t n = 1; n < 1000; n++) {
        short_from_long(name, n, out_11);
        uint32_t lba, off;
        if (!dir_locate(dir_cluster, out_11, &lba, &off)) return true;
    }
    return false;
}

// Enough consecutive free entries for a whole run. A free entry is a deleted one
// or the never-used one that ends the directory, and everything past that is
// free too -- so a run that reaches the end simply continues into the cluster
// added here.
static bool dir_alloc_run(uint32_t dir_cluster, uint32_t count, uint32_t *first_idx) {
    uint32_t index = 0, run = 0, run_start = 0;
    uint32_t last = dir_cluster;

    while (dir_cluster >= 2 && dir_cluster < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            if (!K->sd_read_block(cluster_to_lba(dir_cluster) + s, sector)) return false;
            for (uint32_t e = 0; e < 512; e += 32, index++) {
                if (sector[e] != 0x00 && sector[e] != 0xe5) { run = 0; continue; }
                if (!run) run_start = index;
                if (++run == count) { *first_idx = run_start; return true; }
            }
        }
        last = dir_cluster;
        dir_cluster = fat_get(dir_cluster);
    }

    uint32_t fresh = fat_alloc();
    if (!fresh || !fat_put(last, fresh)) return false;
    for (int i = 0; i < 512; i++) sector[i] = 0;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (!K->sd_write_block(cluster_to_lba(fresh) + s, sector)) return false;
    }
    *first_idx = run ? run_start : index;
    return true;
}

// The fragments, written where dir_alloc_run said they fit.
//
// They lie on the card in reverse: the fragment holding the END of the name is
// stored first, marked with 0x40, and the one holding the beginning sits right
// before the short entry. Thirteen UTF-16 units each, at three separate offsets,
// because the fields had to fit around a layout that was already fixed.
//
// After the name's last character comes one 0x0000 and then 0xffff to the end of
// the fragment. A name that exactly fills its last fragment gets neither, which
// is why the terminator is written by position rather than after the loop.
static bool dir_write_lfn(uint32_t dir_cluster, uint32_t first_idx,
                          const char *name, const char *short_11, uint32_t frags) {
    static const uint8_t off[13] = { 1,3,5,7,9, 14,16,18,20,22,24, 28,30 };
    uint8_t sum = lfn_checksum((const uint8_t *)short_11);

    uint32_t len = 0;
    while (name[len]) len++;

    for (uint32_t k = 0; k < frags; k++) {
        uint32_t seq = frags - k;                  // what lands at first_idx + k
        uint32_t lba, o;
        if (!dir_at(dir_cluster, first_idx + k, &lba, &o)) return false;
        if (!K->sd_read_block(lba, sector)) return false;

        uint8_t *e = &sector[o];
        for (int i = 0; i < 32; i++) e[i] = 0;
        e[0]  = (uint8_t)(seq | (k == 0 ? 0x40u : 0u));
        e[11] = 0x0f;
        e[13] = sum;

        uint32_t base = (seq - 1) * 13u;
        for (uint32_t i = 0; i < 13; i++) {
            uint32_t at = base + i;
            uint16_t u = (at < len) ? (uint16_t)(uint8_t)name[at]
                       : (at == len) ? 0u : 0xffffu;
            wr16(&e[off[i]], u);
        }
        if (!K->sd_write_block(lba, sector)) return false;
    }
    return true;
}

// Delete a whole run, fragments first and the short entry last.
//
// The order is the safe one. Stopping halfway leaves the short entry intact and
// the file reachable under its 8.3 name, which is untidy and harmless. The other
// way round would leave fragments with no entry of their own, and the next file
// created in those slots would wear the dead file's name.
static bool dir_erase_run(uint32_t dir_cluster, uint32_t first, uint32_t last) {
    for (uint32_t i = first; i <= last; i++) {
        uint32_t lba, off;
        if (!dir_at(dir_cluster, i, &lba, &off)) return false;
        if (!K->sd_read_block(lba, sector)) return false;
        sector[off] = 0xe5;
        if (!K->sd_write_block(lba, sector)) return false;
    }
    return true;
}

// Make the entry for a new name: the fragments if it needs them, then the short
// entry, which the caller fills in and writes. Hands back where that entry is.
//
// The short entry goes last on purpose. Fragments followed by nothing are
// skipped by every reader; a short entry followed by nothing is a file.
static bool dir_create(uint32_t dir_cluster, const char *name_83, const char *leaf_long,
                       uint32_t *lba_out, uint32_t *off_out) {
    char short_11[12];
    uint32_t frags = 0;

    if (leaf_long && leaf_long[0] && needs_lfn(leaf_long)) {
        uint32_t len = 0;
        while (leaf_long[len]) len++;
        frags = (len + 12) / 13;
        if (!unique_short(dir_cluster, leaf_long, short_11)) return false;
    } else {
        for (int i = 0; i < 12; i++) short_11[i] = name_83[i];
    }

    uint32_t first = 0;
    if (!dir_alloc_run(dir_cluster, frags + 1, &first)) return false;
    if (frags && !dir_write_lfn(dir_cluster, first, leaf_long, short_11, frags)) return false;

    if (!dir_at(dir_cluster, first + frags, lba_out, off_out)) return false;
    if (!K->sd_read_block(*lba_out, sector)) return false;
    for (int i = 0; i < 11; i++) sector[*off_out + i] = (uint8_t)short_11[i];
    for (int i = 11; i < 32; i++) sector[*off_out + i] = 0;
    return true;
}

bool myrtos_fat_remove(const char *path) {
    myrtos_fat_forget_read_cache();
    if (!mounted) return false;

    uint32_t dir = 0; char name_83[12], leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(path, &dir, name_83, leaf)) return false;

    uint32_t lba, off, first, last;
    if (!dir_find(dir, name_83, leaf, &lba, &off, &first, &last)) return false;
    if (sector[off + 11] & 0x10) return false;          // a directory, not a file

    uint32_t cluster = ((uint32_t)rd16(&sector[off + 20]) << 16) | rd16(&sector[off + 26]);

    // The entries go first, all of them. If the power fails between this and the
    // next line, a directory that no longer names the file leaks clusters; the
    // other order would leave a name pointing at clusters handed to somebody
    // else. Within the run, fragments before the short entry -- see dir_erase_run.
    if (!dir_erase_run(dir, first, last)) return false;

    fat_free_chain(cluster);
    return true;
}

// Give a file another name, which on FAT is an edit of two directory entries
// and touches no data at all: the name lives in the entry and the contents live
// in a cluster chain the entry points at. Copying and deleting would move every
// byte through this machine to achieve the same thing.
//
// It works across directories on this volume for the same reason -- the chain
// does not care which directory names it -- so this is mv as well as rename.
//
// The order is: create the new entry, copy the chain and size into it, then
// erase the old one. A power failure between them leaves two names for one
// chain, which fsck resolves and which the card survives. The other order would
// leave the clusters allocated with nothing naming them, and the file is then
// gone -- so this order risks a duplicate and the other risks the data. See the
// same argument the other way round in myrtos_fat_remove, where erasing first
// is right because there is no second name to be had.
bool myrtos_fat_rename(const char *from, const char *to) {
    myrtos_fat_forget_read_cache();
    if (!mounted) return false;

    uint32_t src_dir = 0; char src_83[12], src_leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(from, &src_dir, src_83, src_leaf)) return false;

    uint32_t lba, off, first, last;
    if (!dir_find(src_dir, src_83, src_leaf, &lba, &off, &first, &last)) return false;

    // Everything the new entry has to carry, taken before anything is written:
    // dir_create reuses the same sector buffer, so reading these afterwards
    // would read whatever it left there.
    uint8_t  attr    = sector[off + 11];
    uint32_t cluster = ((uint32_t)rd16(&sector[off + 20]) << 16) | rd16(&sector[off + 26]);
    uint32_t size    = rd32(&sector[off + 28]);

    uint32_t dst_dir = 0; char dst_83[12], dst_leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(to, &dst_dir, dst_83, dst_leaf)) return false;

    // A destination that exists is refused rather than replaced. Overwriting
    // would have to free the destination's chain, and doing that before the
    // rename has succeeded is how one loses two files instead of renaming one.
    if (dir_find(dst_dir, dst_83, dst_leaf, 0, 0, 0, 0)) return false;

    uint32_t dst_lba, dst_off;
    if (!dir_create(dst_dir, dst_83, dst_leaf, &dst_lba, &dst_off)) return false;

    // dir_create leaves the new entry IN `sector`, with the name written and
    // the rest zeroed, and deliberately does not write it back: the caller
    // fills in what only it knows and writes once. myrtos_fat_write_at does
    // exactly that.
    //
    // Re-reading the sector here threw the name away. What was written back was
    // an entry whose first byte is zero, which every reader takes as the end of
    // the directory -- so the destination never appeared, the source had
    // already been erased, and every call in the chain returned true. That cost
    // a file, and the comment above about which order is safe was beside the
    // point: the order was right and the buffer was wrong.
    sector[dst_off + 11] = attr;
    wr16(&sector[dst_off + 20], (uint16_t)(cluster >> 16));
    wr16(&sector[dst_off + 26], (uint16_t)(cluster & 0xffffu));
    wr32(&sector[dst_off + 28], size);
    if (!K->sd_write_block(dst_lba, sector)) return false;

    // And only now does the old name go. dir_find is repeated because the
    // sector buffer has been used since, and the run it found is what tells
    // this how many entries a long name occupied.
    if (!dir_find(src_dir, src_83, src_leaf, &lba, &off, &first, &last)) return false;
    return dir_erase_run(src_dir, first, last);
}

// Write a slice of a file, creating it and extending it as needed. The mirror of
// myrtos_fat_read_at, and for the same reason: a process has 4 kB for data and
// stack, so a utility streams rather than holding a file in memory.
int32_t myrtos_fat_write_at(const char *path, uint32_t offset,
                            const uint8_t *buf, uint32_t len) {
    myrtos_fat_forget_read_cache();
    if (!mounted || !len) return -1;

    uint32_t dir = 0; char name_83[12], leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(path, &dir, name_83, leaf)) return -1;

    uint32_t lba, off, first_cluster, size;
    if (dir_find(dir, name_83, leaf, &lba, &off, 0, 0)) {
        if (sector[off + 11] & 0x10) return -1;         // a directory
        first_cluster = ((uint32_t)rd16(&sector[off + 20]) << 16) | rd16(&sector[off + 26]);
        size = rd32(&sector[off + 28]);
    } else {
        if (!dir_create(dir, name_83, leaf, &lba, &off)) return -1;
        first_cluster = 0;
        size = 0;
        if (!K->sd_write_block(lba, sector)) return -1;
    }

    const uint32_t bytes_per_cluster = sectors_per_cluster * 512;

    if (!first_cluster) {
        first_cluster = fat_alloc();
        if (!first_cluster) return -1;
    }

    // Walk to the cluster holding `offset`, growing the chain where it ends.
    uint32_t cluster = first_cluster;
    for (uint32_t skip = offset / bytes_per_cluster; skip; skip--) {
        uint32_t next = fat_get(cluster);
        if (next >= 0x0ffffff8) {
            next = fat_alloc();
            if (!next || !fat_put(cluster, next)) return -1;
        }
        cluster = next;
    }

    uint32_t pos = offset % bytes_per_cluster, written = 0;
    while (written < len) {
        for (uint32_t s = pos / 512; s < sectors_per_cluster && written < len; s++) {
            uint32_t dlba = cluster_to_lba(cluster) + s;

            // Read before write: a partial sector must keep the bytes around it.
            // A whole one has nothing to keep, and reading it first doubled the
            // card traffic of every file written.
            const bool whole = pos % 512 == 0 && len - written >= 512;
            if (!whole && !K->sd_read_block(dlba, sector)) return -1;
            for (uint32_t i = pos % 512; i < 512 && written < len; i++) {
                sector[i] = buf[written++];
            }
            if (!K->sd_write_block(dlba, sector)) return -1;
            pos = 0;
        }
        pos = 0;
        if (written < len) {
            uint32_t next = fat_get(cluster);
            if (next >= 0x0ffffff8) {
                next = fat_alloc();
                if (!next || !fat_put(cluster, next)) return -1;
            }
            cluster = next;
        }
    }

    // The directory entry is written last, so a file only ever claims bytes that
    // are already on the card.
    if (!K->sd_read_block(lba, sector)) return -1;
    wr16(&sector[off + 20], (uint16_t)(first_cluster >> 16));
    wr16(&sector[off + 26], (uint16_t)(first_cluster & 0xffff));
    if (offset + len > size) wr32(&sector[off + 28], offset + len);
    sector[off + 11] = 0x20;                             // archive, an ordinary file
    if (!K->sd_write_block(lba, sector)) return -1;

    return (int32_t)len;
}


// Make a directory. A new cluster is zeroed, given its own "." and "..", and
// only then named in the parent -- so a half-made directory is never reachable.
//
// The ".." of a directory whose parent is the root holds zero, not the root's
// cluster number. FAT has said so since the beginning, and resolve_dir turns it
// back into root_cluster on the way up.
bool myrtos_fat_mkdir(const char *path) {
    myrtos_fat_forget_read_cache();
    if (!mounted) return false;

    uint32_t dir = 0;
    char name_83[12], leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(path, &dir, name_83, leaf)) return false;

    uint32_t lba, off;
    if (dir_find(dir, name_83, leaf, &lba, &off, 0, 0)) return false;   // taken

    uint32_t fresh = fat_alloc();
    if (!fresh) return false;

    for (int i = 0; i < 512; i++) sector[i] = 0;
    for (uint32_t s = 0; s < sectors_per_cluster; s++) {
        if (!K->sd_write_block(cluster_to_lba(fresh) + s, sector)) return false;
    }

    for (int i = 0; i < 11; i++) { sector[i] = ' '; sector[32 + i] = ' '; }
    sector[0] = '.';
    sector[11] = 0x10;
    wr16(&sector[20], (uint16_t)(fresh >> 16));
    wr16(&sector[26], (uint16_t)(fresh & 0xffff));
    sector[32] = '.'; sector[33] = '.';
    sector[32 + 11] = 0x10;
    uint32_t up = (dir == root_cluster) ? 0 : dir;
    wr16(&sector[32 + 20], (uint16_t)(up >> 16));
    wr16(&sector[32 + 26], (uint16_t)(up & 0xffff));
    if (!K->sd_write_block(cluster_to_lba(fresh), sector)) return false;

    if (!dir_create(dir, name_83, leaf, &lba, &off)) return false;
    sector[off + 11] = 0x10;
    wr16(&sector[off + 20], (uint16_t)(fresh >> 16));
    wr16(&sector[off + 26], (uint16_t)(fresh & 0xffff));
    return K->sd_write_block(lba, sector);
}

// Remove a directory, provided it is empty. "Empty" means nothing in it but its
// own "." and "..", which every directory has and neither of which counts.
//
// The entry is deleted before the clusters are freed, the same order as for a
// file: the other way round would leave a name pointing at clusters that had
// been handed to somebody else.
bool myrtos_fat_rmdir(const char *path) {
    myrtos_fat_forget_read_cache();
    if (!mounted) return false;

    uint32_t dir = 0;
    char name_83[12], leaf[FAT_LFN_MAX + 1];
    if (!resolve_parent(path, &dir, name_83, leaf)) return false;
    if (name_83[0] == '.') return false;              // never "." or ".."

    uint32_t lba, off, first, last;
    if (!dir_find(dir, name_83, leaf, &lba, &off, &first, &last)) return false;
    if (!(sector[off + 11] & 0x10)) return false;     // a file, not a directory

    uint32_t cluster = ((uint32_t)rd16(&sector[off + 20]) << 16) | rd16(&sector[off + 26]);
    if (cluster < 2 || cluster == root_cluster) return false;

    bool empty = true, done = false;
    uint32_t c = cluster;
    while (!done && c >= 2 && c < 0x0ffffff8) {
        for (uint32_t s = 0; s < sectors_per_cluster && !done; s++) {
            if (!K->sd_read_block(cluster_to_lba(c) + s, sector)) return false;
            for (int e = 0; e < 512; e += 32) {
                if (sector[e] == 0x00) { done = true; break; }   // end of the directory
                if (sector[e] == 0xe5) continue;                 // deleted
                if (sector[e + 11] == 0x0f) continue;            // long-name fragment
                if (sector[e] == '.') continue;                  // "." and ".."
                empty = false;
                done = true;
                break;
            }
        }
        if (done) break;
        c = fat_next_cluster(c);
    }
    if (!empty) return false;

    if (!dir_erase_run(dir, first, last)) return false;

    fat_free_chain(cluster);
    return true;
}

// Take the card again from the beginning. Mounting happens once at startup, so
// a card swapped while the board is running is not seen -- and a swapped card
// needs the whole conversation repeated, not just the boot sector reread, since
// a fresh card comes up idle and knows nothing of what was asked before.
bool myrtos_fat_remount(void) {
    // SDIO first, and the order is the whole point. A card latches into SPI mode
    // the moment it is addressed that way and stays there until the power is
    // cut, so asking afterwards -- as this did -- asks a card that does not
    // speak it any more. Measured on the debugger while this hung: PIO1's clock
    // was toggling, CMD was released to an input, every RX FIFO was empty and
    // the command DMA sat with two words remaining and never moved. The card
    // was being asked correctly and said nothing, which is what a card in SPI
    // mode does.
    //
    // Still here rather than at startup, and now for a second reason as well as
    // the first: the driver has unbounded waits that upstream itself marks
    // "todo not forever". In this process a hang costs one process. Before the
    // scheduler it costs the board, which is what it did.
    if (K->sd_try_sdio() && myrtos_fat_mount()) return true;
    if (!K->sd_init()) return false;
    return myrtos_fat_mount();
}

// See vfs.h. Nothing above needed changing -- every one of these already takes
// an absolute path in this filesystem's own terms, which is exactly what the
// server hands over once it has stripped the volume name off the front.
const myrtos_fsops_t myrtos_fat_ops = {
    .read_at   = myrtos_fat_read_at,
    .write_at  = myrtos_fat_write_at,
    .remove    = myrtos_fat_remove,
    .rename    = myrtos_fat_rename,
    .mkdir     = myrtos_fat_mkdir,
    .rmdir     = myrtos_fat_rmdir,
    .stat_nth  = myrtos_fat_stat_nth,
    .stat      = myrtos_fat_stat,
    .find_nth  = myrtos_fat_find_nth,
    .read_file = myrtos_fat_read_file,
};

// --- WHAT THE KERNEL CALLS -------------------------------------------------
// Entry zero takes the kernel's table and must be called first. Entry two is
// not a function but the volume's operation table, which the file server hands
// straight to myrtos_vfs_add -- so a mounted card is nine function pointers
// into PSRAM, relocated at load like everything else in the module.
static bool fat_lib_init(const myrtos_kernel_api_t *api)
{
    if (!api || api->abi != MYRTOS_KERNEL_API_ABI) return false;
    K = api;
    return true;
}

const myrtos_lib_table_t myrtos_lib = {
    .abi   = MYRTOS_LIB_ABI,
    .count = 5,
    .fn    = {
        (void*)fat_lib_init,          // 0: take the kernel's table
        (void*)myrtos_fat_mount,      // 1: find the volume on the card
        (void*)&myrtos_fat_ops,       // 2: the operations, for myrtos_vfs_add
        (void*)myrtos_fat_stat,       // 3: one named entry
        (void*)myrtos_fat_extent,     // 4: where the volume sits, for usbmsc
    },
};
