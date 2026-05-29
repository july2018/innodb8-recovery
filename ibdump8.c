/*
 * ibdump8.c - Dump InnoDB .ibd file metadata for MySQL 8.0
 *
 * Usage: ibdump8 -f <file.ibd> [-v]
 *
 * Shows:
 *   - Overall file info (size, pages, space_id)
 *   - Per page: type, page_no, level, index_id, n_recs, format, lsn
 *   - Summary of page types
 *
 * Useful before running stream_parser8/c_parser8 to understand the
 * tablespace structure.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <time.h>

#ifdef _WIN32
#  include <io.h>
#else
#  include <unistd.h>
#endif

#include "innodb8_types.h"

static inline uint16_t r2(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static inline uint32_t r4(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint64_t r8(const uint8_t *p) { return ((uint64_t)r4(p)<<32)|r4(p+4); }

static const char *page_type_str(uint16_t t) {
    switch (t) {
    case 0:     return "ALLOCATED";
    case 2:     return "UNDO_LOG";
    case 3:     return "INODE";
    case 4:     return "IBUF_FREE_LIST";
    case 5:     return "IBUF_BITMAP";
    case 6:     return "SYS";
    case 7:     return "TRX_SYS";
    case 8:     return "FSP_HDR";
    case 9:     return "XDES";
    case 10:    return "BLOB";
    case 11:    return "ZBLOB";
    case 12:    return "ZBLOB2";
    case 13:    return "UNKNOWN";
    case 17853: return "SDI_INDEX";
    case 17854: return "RTREE_INDEX";
    case 17855: return "BTREE_INDEX";
    default:    return "?";
    }
}

static void usage(const char *p) {
    fprintf(stderr,
        "Usage: %s -f <file.ibd> [-v]\n"
        "  -v  Verbose: print every page\n", p);
}

int main(int argc, char *argv[]) {
    char infile[1024] = "";
    bool verbose = false;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"-f") && i+1<argc)
            snprintf(infile, sizeof(infile), "%s", argv[++i]);
        else if (!strcmp(argv[i],"-v"))
            verbose = true;
        else if (!strcmp(argv[i],"-h")) { usage(argv[0]); return 0; }
    }

    if (!infile[0]) { usage(argv[0]); return 1; }

    FILE *f = fopen(infile, "rb");
    if (!f) { perror(infile); return 1; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint64_t total = (uint64_t)size / UNIV_PAGE_SIZE;
    printf("File   : %s\n", infile);
    printf("Size   : %ld bytes (%.2f MB)\n", size, size/1048576.0);
    printf("Pages  : %llu\n\n", (unsigned long long)total);

    uint8_t  page[UNIV_PAGE_SIZE];
    uint64_t pno = 0;
    uint64_t type_cnt[65536] = {0};

    /* Table of distinct index_id values */
    uint64_t index_ids[1024];
    uint16_t index_levels[1024];
    uint32_t index_leaf_pages[1024];
    int n_index_ids = 0;

    while (fread(page, 1, UNIV_PAGE_SIZE, f) == UNIV_PAGE_SIZE) {
        uint32_t  page_no  = r4(page + FIL_PAGE_OFFSET);
        uint32_t  space_id = r4(page + FIL_PAGE_SPACE_ID);
        uint16_t  ptype    = r2(page + FIL_PAGE_TYPE);
        uint64_t  lsn      = r8(page + FIL_PAGE_LSN);

        type_cnt[ptype]++;

        bool is_index = (ptype == FIL_PAGE_INDEX ||
                         ptype == FIL_PAGE_SDI   ||
                         ptype == FIL_PAGE_RTREE);

        if (is_index) {
            uint16_t level   = r2(page + PAGE_LEVEL);
            uint64_t idx_id  = r8(page + PAGE_INDEX_ID);
            uint16_t n_recs  = r2(page + PAGE_N_RECS);
            bool     compact = (r2(page + PAGE_N_HEAP) & 0x8000) != 0;
            uint16_t garbage = r2(page + PAGE_GARBAGE);

            /* Track index_ids */
            int found = -1;
            for (int k = 0; k < n_index_ids; k++)
                if (index_ids[k] == idx_id) { found = k; break; }
            if (found < 0 && n_index_ids < 1024) {
                index_ids[n_index_ids] = idx_id;
                index_levels[n_index_ids] = level;
                index_leaf_pages[n_index_ids] = 0;
                found = n_index_ids++;
            }
            if (found >= 0 && level == 0)
                index_leaf_pages[found]++;

            if (verbose) {
                printf("Page %6u  %-12s  Space:%-8u  Level:%-3u  "
                       "IndexID:%-20llu  Recs:%-6u  Fmt:%-10s  LSN:%llu  %s\n",
                       page_no, page_type_str(ptype), space_id,
                       level, (unsigned long long)idx_id, n_recs,
                       compact ? "COMPACT" : "REDUNDANT",
                       (unsigned long long)lsn,
                       garbage > 0 ? "(has deleted)" : "");
            }
        } else if (verbose) {
            printf("Page %6u  %-12s  Space:%-8u  LSN:%llu\n",
                   page_no, page_type_str(ptype), space_id,
                   (unsigned long long)lsn);
        }

        pno++;
    }
    fclose(f);

    /* Summary */
    printf("\n=== Page type distribution ===\n");
    for (int t = 0; t < 65536; t++) {
        if (type_cnt[t] > 0)
            printf("  %-16s (%5d): %llu pages\n",
                   page_type_str((uint16_t)t), t,
                   (unsigned long long)type_cnt[t]);
    }

    if (n_index_ids > 0) {
        printf("\n=== Index summary ===\n");
        printf("  %-20s  %-8s  %s\n", "index_id", "max_level", "leaf_pages");
        for (int k = 0; k < n_index_ids; k++)
            printf("  %-20llu  %-8u  %u\n",
                   (unsigned long long)index_ids[k],
                   index_levels[k],
                   index_leaf_pages[k]);
        printf("\n  Use -I <index_id> with c_parser8 to target a specific index.\n");
    }

    return 0;
}
