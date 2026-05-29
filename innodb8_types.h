/*
 * innodb8_types.h - MySQL 8.0 InnoDB page/record format constants
 *
 * Derived from MySQL 8.0.40 source:
 *   storage/innobase/rem/rec.h
 *   storage/innobase/include/fil0types.h
 *   storage/innobase/include/page0types.h
 */

#ifndef INNODB8_TYPES_H
#define INNODB8_TYPES_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/* ===== Basic types ===== */
typedef uint8_t  byte_t;
typedef uint16_t uint16_t;
typedef uint32_t uint32_t;
typedef uint64_t uint64_t;

/* ===== InnoDB page size ===== */
#define UNIV_PAGE_SIZE      16384   /* 16 KiB default */
#define UNIV_PAGE_SIZE_SHIFT 14

/* ===== FIL page header offsets ===== */
/* MySQL 8.0 fil0types.h */
#define FIL_PAGE_SPACE_OR_CHKSUM    0   /* checksum */
#define FIL_PAGE_OFFSET             4   /* page number */
#define FIL_PAGE_PREV               8   /* previous page */
#define FIL_PAGE_NEXT              12   /* next page */
#define FIL_PAGE_LSN               16   /* LSN of last modification */
#define FIL_PAGE_TYPE              24   /* page type */
#define FIL_PAGE_FILE_FLUSH_LSN    26   /* flush LSN (page 0 only) */
#define FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID 34  /* space id */
#define FIL_PAGE_SPACE_ID          34
#define FIL_PAGE_DATA              38   /* start of page data */
#define FIL_PAGE_DATA_END           8   /* page trailer size */

/* FIL page types (MySQL 8.0) */
#define FIL_PAGE_TYPE_ALLOCATED     0   /* freshly allocated */
#define FIL_PAGE_INDEX          17855   /* B-tree index page */
#define FIL_PAGE_RTREE          17854   /* R-tree index page */
#define FIL_PAGE_SDI            17853   /* SDI index page (8.0 new) */
#define FIL_PAGE_UNDO_LOG           2
#define FIL_PAGE_INODE              3
#define FIL_PAGE_IBUF_FREE_LIST     4
#define FIL_PAGE_IBUF_BITMAP        5
#define FIL_PAGE_TYPE_SYS           6
#define FIL_PAGE_TYPE_TRX_SYS      7
#define FIL_PAGE_TYPE_FSP_HDR       8
#define FIL_PAGE_TYPE_XDES          9
#define FIL_PAGE_TYPE_BLOB         10
#define FIL_PAGE_TYPE_ZBLOB        11
#define FIL_PAGE_TYPE_ZBLOB2       12
#define FIL_PAGE_TYPE_UNKNOWN      13

/* ===== Page header offsets (relative to FIL_PAGE_DATA) ===== */
/* page0types.h: PAGE_HEADER = FSEG_PAGE_DATA = FIL_PAGE_DATA = 38 */
#define PAGE_HEADER        FIL_PAGE_DATA   /* = 38 */
#define PAGE_N_DIR_SLOTS   (PAGE_HEADER + 0)   /* 2 bytes: slots in page dir */
#define PAGE_HEAP_TOP      (PAGE_HEADER + 2)   /* 2 bytes: first free byte */
#define PAGE_N_HEAP        (PAGE_HEADER + 4)   /* 2 bytes: records in heap; bit15=compact flag */
#define PAGE_FREE          (PAGE_HEADER + 6)   /* 2 bytes: head of free list */
#define PAGE_GARBAGE       (PAGE_HEADER + 8)   /* 2 bytes: bytes in deleted recs */
#define PAGE_LAST_INSERT   (PAGE_HEADER + 10)  /* 2 bytes: last insert ptr */
#define PAGE_DIRECTION     (PAGE_HEADER + 12)  /* 2 bytes: insert direction */
#define PAGE_N_DIRECTION   (PAGE_HEADER + 14)  /* 2 bytes: consecutive same-direction inserts */
#define PAGE_N_RECS        (PAGE_HEADER + 16)  /* 2 bytes: user record count */
#define PAGE_MAX_TRX_ID    (PAGE_HEADER + 18)  /* 8 bytes: max trx id */
#define PAGE_LEVEL         (PAGE_HEADER + 26)  /* 2 bytes: B-tree level (0=leaf) */
#define PAGE_INDEX_ID      (PAGE_HEADER + 28)  /* 8 bytes: index id */
/* FSEG headers: 36 bytes total, 2 segments of FSEG_HEADER_SIZE=10 each */
#define FSEG_HEADER_SIZE   10
#define PAGE_BTR_SEG_LEAF  (PAGE_HEADER + 36)
#define PAGE_BTR_SEG_TOP   (PAGE_HEADER + 36 + FSEG_HEADER_SIZE)

/* page data starts at: PAGE_HEADER + 36 + 2*FSEG_HEADER_SIZE = 38 + 36 + 20 = 94 */
#define PAGE_DATA          (PAGE_HEADER + 36 + 2 * FSEG_HEADER_SIZE)  /* = 94 */

/* ===== Record extra bytes ===== */
/* rem/rec.h */
#define REC_N_OLD_EXTRA_BYTES   6   /* REDUNDANT format extra */
#define REC_N_NEW_EXTRA_BYTES   5   /* COMPACT/DYNAMIC/COMPRESSED extra */

/* ===== Infimum / Supremum offsets ===== */
/* COMPACT/DYNAMIC format (new-style) */
#define PAGE_NEW_INFIMUM   (PAGE_DATA + REC_N_NEW_EXTRA_BYTES)        /* = 99 */
#define PAGE_NEW_SUPREMUM  (PAGE_DATA + 2 * REC_N_NEW_EXTRA_BYTES + 8) /* = 112 */
#define PAGE_NEW_SUPREMUM_END (PAGE_NEW_SUPREMUM + 8)                  /* = 120 */

/* REDUNDANT format (old-style) */
#define PAGE_OLD_INFIMUM   (PAGE_DATA + 1 + REC_N_OLD_EXTRA_BYTES)    /* = 101 */
#define PAGE_OLD_SUPREMUM  (PAGE_DATA + 2 + 2 * REC_N_OLD_EXTRA_BYTES + 8) /* = 120 */
#define PAGE_OLD_SUPREMUM_END (PAGE_OLD_SUPREMUM + 9)                  /* = 129 */

/* ===== Record header bit fields ===== */
/* Offsets measured backward from record origin (rec points to first data byte) */
#define REC_NEXT            2   /* 2 bytes: next record offset */
#define REC_NEXT_MASK       0xFFFF
#define REC_NEXT_SHIFT      0

/* NEW style (COMPACT/DYNAMIC) */
#define REC_NEW_STATUS      3   /* 1 byte: status bits [2:0] */
#define REC_NEW_STATUS_MASK 0x07
#define REC_NEW_HEAP_NO     4   /* 2 bytes: heap_no in [15:3] */
#define REC_HEAP_NO_MASK    0xFFF8
#define REC_HEAP_NO_SHIFT   3
#define REC_NEW_N_OWNED     5   /* 1 byte: n_owned in [3:0] */
#define REC_NEW_INFO_BITS   5   /* 1 byte: info bits in [7:4] */

/* OLD style (REDUNDANT) */
#define REC_OLD_SHORT       3   /* 1 byte: 1-byte offset flag */
#define REC_OLD_SHORT_MASK  0x01
#define REC_OLD_N_FIELDS    4   /* 2 bytes: num fields in [10:1] */
#define REC_OLD_N_FIELDS_MASK  0x07FE
#define REC_OLD_N_FIELDS_SHIFT  1
#define REC_OLD_HEAP_NO     5   /* 2 bytes: heap_no in [15:3] */
#define REC_OLD_N_OWNED     6   /* 1 byte: n_owned in [3:0] */
#define REC_OLD_INFO_BITS   6   /* 1 byte: info bits in [7:4] */

/* Info bits (high nibble) */
#define REC_INFO_BITS_MASK      0xF0
#define REC_INFO_BITS_SHIFT     0
#define REC_INFO_MIN_REC_FLAG   0x10  /* leftmost non-leaf page record */
#define REC_INFO_DELETED_FLAG   0x20  /* delete-mark */
#define REC_INFO_VERSION_FLAG   0x40  /* MySQL 8.0: row has version */
#define REC_INFO_INSTANT_FLAG   0x80  /* MySQL 8.0: instant ADD COLUMN record */
#define REC_N_OWNED_MASK        0x0F

/* Record status values (new-style) */
#define REC_STATUS_ORDINARY     0   /* leaf node user record */
#define REC_STATUS_NODE_PTR     1   /* internal node pointer */
#define REC_STATUS_INFIMUM      2
#define REC_STATUS_SUPREMUM     3

/* Offset flags in rec_get_offsets() results */
#define REC_OFFS_COMPACT    (1U << 31)
#define REC_OFFS_SQL_NULL   (1U << 31)
#define REC_OFFS_EXTERNAL   (1 << 30)
#define REC_OFFS_DEFAULT    (1 << 29)  /* MySQL 8.0: instant default */
#define REC_OFFS_DROP       (1 << 28)  /* MySQL 8.0: instant drop */
#define REC_OFFS_MASK       (REC_OFFS_DROP - 1)

/* REDUNDANT 1-byte / 2-byte offset null/extern flags */
#define REC_1BYTE_SQL_NULL_MASK  0x80
#define REC_2BYTE_SQL_NULL_MASK  0x8000
#define REC_2BYTE_EXTERN_MASK    0x4000

/* ===== Misc ===== */
#define FIL_NULL            0xFFFFFFFFUL   /* end-of-list marker */
#define UNIV_SQL_NULL       ((uint32_t)0xFFFFFFFF)
#define PAGE_HEAP_NO_INFIMUM  0
#define PAGE_HEAP_NO_SUPREMUM 1
#define PAGE_HEAP_NO_USER_LOW 2

/* MySQL 8.0: 1-byte/2-byte n_fields encoding (instant ADD COLUMN) */
#define REC_N_FIELDS_TWO_BYTES_FLAG  0x80
#define REC_N_FIELDS_ONE_BYTE_MAX    0x7F

/* ===== Column types for CREATE TABLE parsing ===== */
typedef enum {
    COL_TINYINT = 0,
    COL_SMALLINT,
    COL_MEDIUMINT,
    COL_INT,
    COL_BIGINT,
    COL_FLOAT,
    COL_DOUBLE,
    COL_DECIMAL,
    COL_DATE,
    COL_DATETIME,
    COL_TIMESTAMP,
    COL_TIME,
    COL_YEAR,
    COL_CHAR,
    COL_VARCHAR,
    COL_BINARY,
    COL_VARBINARY,
    COL_TINYBLOB,
    COL_BLOB,
    COL_MEDIUMBLOB,
    COL_LONGBLOB,
    COL_TINYTEXT,
    COL_TEXT,
    COL_MEDIUMTEXT,
    COL_LONGTEXT,
    COL_BIT,
    COL_ENUM,
    COL_SET,
    COL_JSON,
    COL_UNKNOWN
} col_type_t;

/* Column descriptor */
#define MAX_COL_NAME  256
#define MAX_COLS      512

typedef struct {
    char      name[MAX_COL_NAME];
    col_type_t type;
    uint32_t  fixed_len;    /* 0 = variable */
    uint32_t  max_len;
    bool      nullable;
    bool      unsigned_flag;
    int       prec;         /* DECIMAL/FLOAT precision */
    int       scale;        /* DECIMAL scale */
} col_def_t;

typedef struct {
    char      table_name[MAX_COL_NAME];
    col_def_t cols[MAX_COLS];
    int       n_cols;
    int       n_nullable;
    uint32_t  min_rec_len;  /* minimum record data size */
    uint32_t  max_rec_len;  /* maximum record data size */
    bool      is_compact;   /* COMPACT/DYNAMIC/COMPRESSED (not REDUNDANT) */
} table_def_t;

#endif /* INNODB8_TYPES_H */
