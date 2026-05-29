/*
 * stream_parser8.c - InnoDB .ibd file page scanner for MySQL 8.0
 *
 * Usage: stream_parser8 -f <file.ibd> [-d] [-o <dir>]
 *
 * Scans every 16KB page in an InnoDB tablespace file and writes
 * each page type into a separate subdirectory:
 *   pages-<file>/FIL_PAGE_INDEX/      B-tree index pages
 *   pages-<file>/FIL_PAGE_BLOB/       BLOB pages
 *   pages-<file>/FIL_PAGE_SDI/        SDI pages (MySQL 8.0)
 *   pages-<file>/FIL_PAGE_INODE/      inode pages
 *   etc.
 *
 * Modelled after twindb/undrop-for-innodb stream_parser.c
 * but updated for MySQL 8.0.40 page format.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  define mkdir(p, m)  _mkdir(p)
#  define snprintf     _snprintf
#else
#  include <unistd.h>
#endif

#include "innodb8_types.h"

/* ===== helpers: big-endian reads ===== */
static inline uint16_t read2(const uint8_t *p) {
    return ((uint16_t)p[0] << 8) | p[1];
}
static inline uint32_t read4(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
         | ((uint32_t)p[2] << 8) | p[3];
}
static inline uint64_t read8(const uint8_t *p) {
    return ((uint64_t)read4(p) << 32) | read4(p + 4);
}

/* ===== InnoDB CRC32c (polynomial 0x82F63B78) ===== */
static uint32_t crc32c_table[256];
static bool     crc32c_inited = false;

static void crc32c_init(void) {
    uint32_t i, j, crc;
    for (i = 0; i < 256; i++) {
        crc = i;
        for (j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (crc & 1 ? 0x82F63B78 : 0);
        crc32c_table[i] = crc;
    }
    crc32c_inited = true;
}

static uint32_t crc32c(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    if (!crc32c_inited) crc32c_init();
    for (size_t i = 0; i < len; i++)
        crc = (crc >> 8) ^ crc32c_table[(crc ^ data[i]) & 0xFF];
    return crc ^ 0xFFFFFFFF;
}

/*
 * Verify InnoDB page checksum.
 * MySQL 8.0 uses CRC32c for the page body (bytes 4..page_size-8)
 * and stores it at bytes 0..3 and last 4 bytes.
 */
static bool page_checksum_ok(const uint8_t *page) {
    uint32_t stored   = read4(page + FIL_PAGE_SPACE_OR_CHKSUM);
    uint32_t computed = crc32c(page + 4, UNIV_PAGE_SIZE - FIL_PAGE_DATA_END - 4);
    /* Also accept innodb_checksum_algorithm=none (stored==0) */
    if (stored == 0) return true;
    return (stored == computed);
}

/* ===== page type name ===== */
static const char *page_type_name(uint16_t type) {
    switch (type) {
    case FIL_PAGE_INDEX:             return "FIL_PAGE_INDEX";
    case FIL_PAGE_RTREE:             return "FIL_PAGE_RTREE";
    case FIL_PAGE_SDI:               return "FIL_PAGE_SDI";
    case FIL_PAGE_UNDO_LOG:          return "FIL_PAGE_UNDO_LOG";
    case FIL_PAGE_INODE:             return "FIL_PAGE_INODE";
    case FIL_PAGE_IBUF_FREE_LIST:    return "FIL_PAGE_IBUF_FREE_LIST";
    case FIL_PAGE_TYPE_ALLOCATED:    return "FIL_PAGE_ALLOCATED";
    case FIL_PAGE_IBUF_BITMAP:       return "FIL_PAGE_IBUF_BITMAP";
    case FIL_PAGE_TYPE_SYS:          return "FIL_PAGE_TYPE_SYS";
    case FIL_PAGE_TYPE_TRX_SYS:      return "FIL_PAGE_TRX_SYS";
    case FIL_PAGE_TYPE_FSP_HDR:      return "FIL_PAGE_FSP_HDR";
    case FIL_PAGE_TYPE_XDES:         return "FIL_PAGE_XDES";
    case FIL_PAGE_TYPE_BLOB:         return "FIL_PAGE_BLOB";
    case FIL_PAGE_TYPE_ZBLOB:        return "FIL_PAGE_ZBLOB";
    case FIL_PAGE_TYPE_ZBLOB2:       return "FIL_PAGE_ZBLOB2";
    default:                         return "FIL_PAGE_UNKNOWN";
    }
}

/* ===== mkdir -p ===== */
static int mkdirp(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    size_t len = strlen(tmp);
    for (size_t i = 1; i < len; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            mkdir(tmp, 0755);
            tmp[i] = '/';
        }
    }
    return mkdir(tmp, 0755);
}

/* ===== options ===== */
static bool opt_deleted_only = false;   /* -d: only pages with deleted recs */
static bool opt_verbose      = false;   /* -v: verbose output */
static char opt_outdir[1024] = "";      /* -o output dir */

static void usage(const char *prog) {
    fprintf(stderr,
        "MySQL 8.0 InnoDB Page Scanner\n"
        "Usage: %s -f <file.ibd> [-d] [-v] [-o <outdir>]\n"
        "  -f <file>    InnoDB tablespace file\n"
        "  -d           Only extract pages that may contain deleted records\n"
        "  -v           Verbose output\n"
        "  -o <dir>     Output directory (default: pages-<basename>)\n\n"
        "Output pages are written as:\n"
        "  <outdir>/FIL_PAGE_INDEX/<page_no>.page\n"
        "  <outdir>/FIL_PAGE_BLOB/<page_no>.page  (etc.)\n",
        prog);
}

/* ===== main ===== */
int main(int argc, char *argv[]) {
    char infile[1024] = "";
    int opt;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc)
            snprintf(infile, sizeof(infile), "%s", argv[++i]);
        else if (strcmp(argv[i], "-d") == 0)
            opt_deleted_only = true;
        else if (strcmp(argv[i], "-v") == 0)
            opt_verbose = true;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            snprintf(opt_outdir, sizeof(opt_outdir), "%s", argv[++i]);
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]); return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            usage(argv[0]); return 1;
        }
    }

    if (!infile[0]) {
        fprintf(stderr, "Error: -f <file> is required\n");
        usage(argv[0]);
        return 1;
    }

    /* Derive output directory from input filename if not specified */
    if (!opt_outdir[0]) {
        const char *base = strrchr(infile, '/');
        if (!base) base = strrchr(infile, '\\');
        base = base ? base + 1 : infile;
        snprintf(opt_outdir, sizeof(opt_outdir), "pages-%s", base);
    }

    FILE *fp = fopen(infile, "rb");
    if (!fp) {
        perror(infile);
        return 1;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint64_t total_pages = (uint64_t)file_size / UNIV_PAGE_SIZE;
    fprintf(stderr, "File: %s  Size: %ld bytes  Pages: %llu\n",
            infile, file_size, (unsigned long long)total_pages);

    uint8_t page[UNIV_PAGE_SIZE];
    uint64_t page_no_scanned = 0;
    uint64_t index_pages = 0, blob_pages = 0, other_pages = 0;
    uint64_t bad_checksum = 0;

    /* Stats per page type */
    uint64_t type_counts[65536] = {0};

    /* Create base output dir */
    mkdirp(opt_outdir);

    while (fread(page, 1, UNIV_PAGE_SIZE, fp) == UNIV_PAGE_SIZE) {
        uint32_t page_no  = read4(page + FIL_PAGE_OFFSET);
        uint32_t space_id = read4(page + FIL_PAGE_SPACE_ID);
        uint16_t page_type = read2(page + FIL_PAGE_TYPE);
        uint64_t lsn      = read8(page + FIL_PAGE_LSN);

        /* For index pages, extract more info */
        uint16_t level = 0;
        uint64_t index_id = 0;
        uint16_t n_recs = 0;
        bool     is_compact = false;
        bool     has_deleted = false;

        if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI ||
            page_type == FIL_PAGE_RTREE) {
            level    = read2(page + PAGE_LEVEL);
            index_id = read8(page + PAGE_INDEX_ID);
            n_recs   = read2(page + PAGE_N_RECS);
            /* Compact flag: bit 15 of PAGE_N_HEAP */
            uint16_t n_heap = read2(page + PAGE_N_HEAP);
            is_compact = (n_heap & 0x8000) != 0;

            /* Check for deleted records: PAGE_GARBAGE != 0 or
               PAGE_FREE != 0 means there are delete-marked slots */
            uint16_t garbage = read2(page + PAGE_GARBAGE);
            uint16_t free    = read2(page + PAGE_FREE);
            has_deleted = (garbage > 0 || free > 0);
        }

        /* Optionally skip non-deleted pages */
        if (opt_deleted_only && !has_deleted &&
            (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI)) {
            page_no_scanned++;
            continue;
        }

        /* Write page to output file */
        char type_dir[2048];
        snprintf(type_dir, sizeof(type_dir), "%s/%s", opt_outdir,
                 page_type_name(page_type));
        mkdirp(type_dir);

        char outpath[2048];
        snprintf(outpath, sizeof(outpath), "%s/%010u.page",
                 type_dir, page_no_scanned);

        FILE *out = fopen(outpath, "wb");
        if (out) {
            fwrite(page, 1, UNIV_PAGE_SIZE, out);
            fclose(out);
        }

        type_counts[page_type]++;
        if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI)
            index_pages++;
        else if (page_type == FIL_PAGE_TYPE_BLOB  ||
                 page_type == FIL_PAGE_TYPE_ZBLOB ||
                 page_type == FIL_PAGE_TYPE_ZBLOB2)
            blob_pages++;
        else
            other_pages++;

        if (opt_verbose) {
            fprintf(stderr,
                "Page %6u  Type: %-26s  Space: %u  LSN: %llu",
                page_no_scanned, page_type_name(page_type),
                space_id, (unsigned long long)lsn);
            if (page_type == FIL_PAGE_INDEX || page_type == FIL_PAGE_SDI) {
                fprintf(stderr, "  Level: %u  Recs: %u  Fmt: %s  Deleted: %s",
                    level, n_recs, is_compact ? "COMPACT" : "REDUNDANT",
                    has_deleted ? "YES" : "no");
            }
            fprintf(stderr, "\n");
        }

        page_no_scanned++;
    }

    fclose(fp);

    fprintf(stderr, "\n=== Summary ===\n");
    fprintf(stderr, "Total pages scanned : %llu\n", (unsigned long long)page_no_scanned);
    fprintf(stderr, "Index pages         : %llu\n", (unsigned long long)index_pages);
    fprintf(stderr, "BLOB pages          : %llu\n", (unsigned long long)blob_pages);
    fprintf(stderr, "Other pages         : %llu\n", (unsigned long long)other_pages);
    fprintf(stderr, "Output directory    : %s\n", opt_outdir);

    return 0;
}
