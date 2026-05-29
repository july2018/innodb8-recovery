/*
 * c_parser8.c - InnoDB record parser for MySQL 8.0
 *
 * Usage:
 *   c_parser8 -f <page_dir_or_file> -t <table.sql> [-o <out.tsv>]
 *             [-D] [-U] [-d] [-v] [-I <index_id>]
 *
 * Options:
 *   -f <path>     Directory of .page files (from stream_parser8) OR single .ibd
 *   -t <sql>      CREATE TABLE SQL file
 *   -o <out>      Output TSV file (default: stdout)
 *   -D            Recover deleted records only
 *   -U            Recover undeleted records only (default)
 *   -d            Skip pages with no deleted records
 *   -v            Verbose debug
 *   -I <id>       Only process pages with this index_id (hex or decimal)
 *   -4            Force ROW_FORMAT=REDUNDANT
 *   -5            Force ROW_FORMAT=COMPACT/DYNAMIC (default: auto-detect)
 *
 * Core algorithm (mirrors undrop-for-innodb c_parser.c):
 *   1. For each InnoDB page:
 *      a. Detect row format (compact flag in PAGE_N_HEAP)
 *      b. Walk the active record chain from infimum→supremum
 *      c. Also scan "garbage" (free list) for deleted records
 *      d. For each candidate record offset, validate then decode
 *   2. MySQL 8.0 specific handling:
 *      - REC_INFO_VERSION_FLAG: record has explicit version byte
 *      - REC_INFO_INSTANT_FLAG: instant ADD COLUMN (extra field count)
 *      - n_fields encoding uses 1-byte or 2-byte form
 *      - DYNAMIC format: all off-page LOB, no prefix stored inline
 *
 * Build with: gcc -O2 -o c_parser8 c_parser8.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <ctype.h>
#include <float.h>
#include <limits.h>

#ifdef _WIN32
#  include <direct.h>
#  include <io.h>
#  include <windows.h>
#else
#  include <unistd.h>
#  include <dirent.h>
#endif

#include "innodb8_types.h"

/* ===== Table definitions (loaded from generated table_defs.h or at runtime) ===== */
/* We store table def here inline; caller sets it via parse_create_sql() */

#define MAX_TABLES  8

static col_def_t   g_cols[MAX_TABLES][MAX_COLS];
static table_def_t g_tables[MAX_TABLES];
static int         g_n_tables = 0;

/* ===== Options ===== */
static bool opt_deleted_only    = false;  /* -D */
static bool opt_undeleted_only  = true;   /* -U (default) */
static bool opt_skip_nodel_page = false;  /* -d */
static bool opt_verbose         = false;  /* -v */
static int  opt_force_format    = 0;      /* -4=REDUNDANT -5=COMPACT */
static uint64_t opt_index_id    = 0;      /* -I */
static bool opt_filter_index    = false;
static char opt_outfile[1024]   = "";     /* -o */
static FILE *f_out              = NULL;

/* ===== Statistics ===== */
static uint64_t g_pages_scanned  = 0;
static uint64_t g_pages_index    = 0;
static uint64_t g_recs_dumped    = 0;
static uint64_t g_recs_expected  = 0;

/* ===== Big-endian helpers ===== */
static inline uint8_t  u8(const uint8_t *p)  { return p[0]; }
static inline uint16_t u16(const uint8_t *p) { return ((uint16_t)p[0]<<8)|p[1]; }
static inline uint32_t u32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}
static inline uint64_t u64(const uint8_t *p) {
    return ((uint64_t)u32(p)<<32)|u32(p+4);
}
/* Read signed little-endian (for InnoDB INT storage with sign flip) */
static inline int32_t read_int_innodb(const uint8_t *p, int len) {
    /* InnoDB stores INT with high bit flipped (unsigned offset representation) */
    if (len == 1) return (int8_t)((int8_t)(p[0] ^ 0x80));
    if (len == 2) return (int16_t)(u16(p) ^ 0x8000);
    if (len == 3) {
        uint32_t v = ((uint32_t)p[0]<<16)|((uint32_t)p[1]<<8)|p[2];
        return (int32_t)(v ^ 0x800000) - 0x800000;
    }
    if (len == 4) return (int32_t)(u32(p) ^ 0x80000000UL);
    if (len == 8) return (int32_t)(u64(p) ^ 0x8000000000000000ULL);
    return 0;
}
static inline int64_t read_int64_innodb(const uint8_t *p) {
    return (int64_t)(u64(p) ^ 0x8000000000000000ULL);
}

/* ===== Record header read helpers ===== */
/* rec points to first data byte; header is at negative offsets */
static inline uint8_t  rec_byte(const uint8_t *rec, int off) { return rec[-off]; }
static inline uint16_t rec_u16(const uint8_t *rec, int off) {
    return ((uint16_t)rec[-off] << 8) | rec[-(off-1)];
}

static inline uint16_t rec_get_next_offs_new(const uint8_t *rec) {
    /* REC_NEXT=2: signed 16-bit relative offset */
    int16_t delta = (int16_t)u16(rec - REC_NEXT);
    return delta;
}

static inline uint16_t rec_get_next_offs_old(const uint8_t *rec) {
    return u16(rec - REC_NEXT);
}

static inline bool rec_get_deleted_flag(const uint8_t *rec, bool comp) {
    uint8_t info = comp
        ? rec_byte(rec, REC_NEW_INFO_BITS)
        : rec_byte(rec, REC_OLD_INFO_BITS);
    return (info & REC_INFO_DELETED_FLAG) != 0;
}

static inline uint8_t rec_get_info_bits(const uint8_t *rec, bool comp) {
    return comp ? rec_byte(rec, REC_NEW_INFO_BITS)
                : rec_byte(rec, REC_OLD_INFO_BITS);
}

static inline uint8_t rec_get_status(const uint8_t *rec) {
    return rec_byte(rec, REC_NEW_STATUS) & REC_NEW_STATUS_MASK;
}

static inline uint16_t rec_get_heap_no_new(const uint8_t *rec) {
    return (u16(rec - REC_NEW_HEAP_NO) & REC_HEAP_NO_MASK) >> REC_HEAP_NO_SHIFT;
}

/* ===== Page helpers ===== */
static inline bool page_is_compact(const uint8_t *page) {
    return (u16(page + PAGE_N_HEAP) & 0x8000) != 0;
}

static inline uint16_t page_get_infimum_offs(bool comp) {
    return comp ? PAGE_NEW_INFIMUM : PAGE_OLD_INFIMUM;
}

static inline uint16_t page_get_supremum_offs(bool comp) {
    return comp ? PAGE_NEW_SUPREMUM : PAGE_OLD_SUPREMUM;
}

/* ===== CREATE TABLE parser ===== */
/*
 * Minimal column type parser — same logic as create_defs_parser8.c
 * but embedded here so c_parser8 is self-contained.
 */

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) e--;
    *e = '\0';
    return s;
}

static col_type_t type_from_name(const char *n) {
    if (!strcasecmp(n,"TINYINT"))   return COL_TINYINT;
    if (!strcasecmp(n,"SMALLINT"))  return COL_SMALLINT;
    if (!strcasecmp(n,"MEDIUMINT")) return COL_MEDIUMINT;
    if (!strcasecmp(n,"INTEGER"))   return COL_INT;
    if (!strcasecmp(n,"INT"))       return COL_INT;
    if (!strcasecmp(n,"BIGINT"))    return COL_BIGINT;
    if (!strcasecmp(n,"FLOAT"))     return COL_FLOAT;
    if (!strcasecmp(n,"DOUBLE"))    return COL_DOUBLE;
    if (!strcasecmp(n,"REAL"))      return COL_DOUBLE;
    if (!strcasecmp(n,"DECIMAL"))   return COL_DECIMAL;
    if (!strcasecmp(n,"NUMERIC"))   return COL_DECIMAL;
    if (!strcasecmp(n,"DATE"))      return COL_DATE;
    if (!strcasecmp(n,"DATETIME"))  return COL_DATETIME;
    if (!strcasecmp(n,"TIMESTAMP")) return COL_TIMESTAMP;
    if (!strcasecmp(n,"TIME"))      return COL_TIME;
    if (!strcasecmp(n,"YEAR"))      return COL_YEAR;
    if (!strcasecmp(n,"CHAR"))      return COL_CHAR;
    if (!strcasecmp(n,"VARCHAR"))   return COL_VARCHAR;
    if (!strcasecmp(n,"BINARY"))    return COL_BINARY;
    if (!strcasecmp(n,"VARBINARY")) return COL_VARBINARY;
    if (!strcasecmp(n,"TINYBLOB"))  return COL_TINYBLOB;
    if (!strcasecmp(n,"TINYTEXT"))  return COL_TINYTEXT;
    if (!strcasecmp(n,"BLOB"))      return COL_BLOB;
    if (!strcasecmp(n,"TEXT"))      return COL_TEXT;
    if (!strcasecmp(n,"MEDIUMBLOB"))return COL_MEDIUMBLOB;
    if (!strcasecmp(n,"MEDIUMTEXT"))return COL_MEDIUMTEXT;
    if (!strcasecmp(n,"LONGBLOB"))  return COL_LONGBLOB;
    if (!strcasecmp(n,"LONGTEXT"))  return COL_LONGTEXT;
    if (!strcasecmp(n,"BIT"))       return COL_BIT;
    if (!strcasecmp(n,"ENUM"))      return COL_ENUM;
    if (!strcasecmp(n,"SET"))       return COL_SET;
    if (!strcasecmp(n,"JSON"))      return COL_JSON;
    return COL_UNKNOWN;
}

/*
 * Parse one column definition line (already stripped of trailing comma).
 * Returns 1 if a column was successfully parsed.
 */
static int parse_col_line(char *line, col_def_t *c) {
    memset(c, 0, sizeof(*c));
    c->nullable = true;

    char *p = trim(line);

    /* Skip table constraints */
    if (!strncasecmp(p,"PRIMARY",7) || !strncasecmp(p,"KEY",3) ||
        !strncasecmp(p,"INDEX",5)   || !strncasecmp(p,"UNIQUE",6) ||
        !strncasecmp(p,"CONSTRAINT",10) || !strncasecmp(p,"FULLTEXT",8) ||
        !strncasecmp(p,"SPATIAL",7) || !strncasecmp(p,"CHECK",5))
        return 0;

    /* Column name */
    char name[MAX_COL_NAME] = "";
    if (*p == '`' || *p == '"') {
        char q = *p++;
        char *e = strchr(p, q);
        if (!e) return 0;
        int l = e - p < MAX_COL_NAME - 1 ? e - p : MAX_COL_NAME - 1;
        strncpy(name, p, l);
        p = e + 1;
    } else {
        char *e = p;
        while (*e && !isspace((unsigned char)*e)) e++;
        int l = e - p < MAX_COL_NAME - 1 ? e - p : MAX_COL_NAME - 1;
        strncpy(name, p, l);
        p = e;
    }
    if (!name[0]) return 0;
    snprintf(c->name, MAX_COL_NAME, "%s", name);
    p = trim(p);

    /* Type */
    char tname[64] = "";
    char *te = p;
    while (*te && *te != '(' && !isspace((unsigned char)*te)) te++;
    int tl = te - p < 63 ? te - p : 63;
    strncpy(tname, p, tl);
    for (int i = 0; tname[i]; i++) tname[i] = toupper((unsigned char)tname[i]);
    p = te;

    c->type = type_from_name(tname);
    if (c->type == COL_UNKNOWN) return 0;

    /* Params (len[,scale]) */
    int p1 = 0, p2 = 0, has_p = 0;
    p = trim(p);
    if (*p == '(') {
        p++; has_p = 1;
        p1 = atoi(p);
        while (*p && *p != ',' && *p != ')') p++;
        if (*p == ',') { p++; p2 = atoi(p); }
        while (*p && *p != ')') p++;
        if (*p == ')') p++;
    }

    /* Flags */
    char rest[512]; snprintf(rest, 512, "%s", p);
    for (int i = 0; rest[i]; i++) rest[i] = toupper((unsigned char)rest[i]);
    if (strstr(rest,"UNSIGNED"))  c->unsigned_flag = true;
    if (strstr(rest,"NOT NULL"))  c->nullable = false;

    /* Sizes */
    switch (c->type) {
    case COL_TINYINT:   c->fixed_len=1; break;
    case COL_SMALLINT:  c->fixed_len=2; break;
    case COL_MEDIUMINT: c->fixed_len=3; break;
    case COL_INT:       c->fixed_len=4; break;
    case COL_BIGINT:    c->fixed_len=8; break;
    case COL_FLOAT:     c->fixed_len=4; break;
    case COL_DOUBLE:    c->fixed_len=8; break;
    case COL_DATE:      c->fixed_len=3; break;
    case COL_YEAR:      c->fixed_len=1; break;
    case COL_TIME:      c->fixed_len=3; break;
    case COL_TIMESTAMP: c->fixed_len=4; break;
    case COL_DATETIME:  c->fixed_len=5; break;
    case COL_DECIMAL: {
        int prec = has_p?p1:10, scale=has_p?p2:0;
        c->prec=prec; c->scale=scale;
        int ib=(prec-scale)/9*4+((prec-scale)%9+1)/2;
        int fb=scale/9*4+(scale%9+1)/2;
        c->fixed_len=ib+fb;
        break;
    }
    case COL_BIT:
        c->fixed_len=(has_p?p1:1+7)/8;
        break;
    case COL_CHAR: case COL_BINARY:
        c->fixed_len=has_p?p1:1; c->max_len=c->fixed_len; break;
    case COL_VARCHAR: case COL_VARBINARY:
        c->fixed_len=0; c->max_len=has_p?p1:255; break;
    case COL_TINYBLOB: case COL_TINYTEXT:
        c->fixed_len=0; c->max_len=255; break;
    case COL_BLOB: case COL_TEXT:
        c->fixed_len=0; c->max_len=65535; break;
    case COL_MEDIUMBLOB: case COL_MEDIUMTEXT:
        c->fixed_len=0; c->max_len=16777215; break;
    case COL_LONGBLOB: case COL_LONGTEXT: case COL_JSON:
        c->fixed_len=0; c->max_len=0xFFFFFFFFU; break;
    case COL_ENUM:
        c->fixed_len=2; c->max_len=2; break;
    case COL_SET:
        c->fixed_len=8; c->max_len=8; break;
    default:
        c->fixed_len=0; c->max_len=65535; break;
    }
    return 1;
}

/*
 * Load table definition from a CREATE TABLE SQL file.
 * Returns index into g_tables, or -1 on failure.
 */
static int load_create_sql(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); return -1; }

    /* Read entire file */
    char sql[65536]; size_t pos=0; char line[4096];
    while (fgets(line,sizeof(line),f) && pos<sizeof(sql)-1) {
        size_t ll=strlen(line);
        memcpy(sql+pos,line,ll); pos+=ll;
    }
    sql[pos]='\0';
    fclose(f);

    if (g_n_tables >= MAX_TABLES) { fprintf(stderr,"Too many tables\n"); return -1; }
    int tidx = g_n_tables;
    table_def_t *td = &g_tables[tidx];
    memset(td, 0, sizeof(*td));

    /* table name */
    char sqlup[65536];
    strncpy(sqlup, sql, sizeof(sqlup)-1);
    for (int i=0;sqlup[i];i++) sqlup[i]=toupper((unsigned char)sqlup[i]);
    char *ct = strstr(sqlup,"CREATE");
    if (!ct) { fprintf(stderr,"No CREATE in %s\n",path); return -1; }
    char *tp = strstr(ct,"TABLE");
    if (!tp) { fprintf(stderr,"No TABLE in %s\n",path); return -1; }
    char *rp = sql + (tp - sqlup) + 5;
    rp = trim(rp);
    if (!strncasecmp(rp,"IF",2)) {
        rp+=2; rp=trim(rp);
        if (!strncasecmp(rp,"NOT",3)) { rp+=3; rp=trim(rp); }
        if (!strncasecmp(rp,"EXISTS",6)) { rp+=6; rp=trim(rp); }
    }
    char tname[MAX_COL_NAME]="";
    if (*rp=='`'||*rp=='"') {
        char q=*rp++;
        char *e=strchr(rp,q);
        if (e) { int l=(int)(e-rp)<MAX_COL_NAME-1?(int)(e-rp):MAX_COL_NAME-1;
                 strncpy(tname,rp,l); }
    } else {
        char *e=rp;
        while(*e&&*e!='('&&!isspace((unsigned char)*e)) e++;
        int l=(int)(e-rp)<MAX_COL_NAME-1?(int)(e-rp):MAX_COL_NAME-1;
        strncpy(tname,rp,l);
    }
    char *dot=strchr(tname,'.'); if(dot) memmove(tname,dot+1,strlen(dot));
    snprintf(td->table_name, MAX_COL_NAME, "%s", tname);

    /* Find column list */
    char *cs = strchr(sql+(tp-sqlup),'(');
    if (!cs) { fprintf(stderr,"No ( in %s\n",path); return -1; }
    cs++;

    /* Parse columns */
    char buf[65536]; strncpy(buf,cs,sizeof(buf)-1);
    char *p=buf; int depth=1; char cline[2048]; int ci=0;
    td->n_cols=0;
    while(*p&&depth>0) {
        if(*p=='(') depth++;
        else if(*p==')') { depth--; if(depth==0) break; }
        if(*p==','&&depth==1) {
            cline[ci]='\0';
            col_def_t c;
            if(parse_col_line(trim(cline),&c) && td->n_cols<MAX_COLS) {
                g_cols[tidx][td->n_cols] = c;
                td->n_cols++;
            }
            ci=0;
        } else {
            if(ci<(int)sizeof(cline)-1) cline[ci++]=*p;
        }
        p++;
    }
    if(ci>0) {
        cline[ci]='\0';
        col_def_t c;
        if(parse_col_line(trim(cline),&c) && td->n_cols<MAX_COLS) {
            g_cols[tidx][td->n_cols]=c; td->n_cols++;
        }
    }

    if(td->n_cols==0) { fprintf(stderr,"No columns parsed in %s\n",path); return -1; }

    /* Link cols pointer */
    td->cols[0] = g_cols[tidx][0]; /* will be accessed via index */

    /* Count nullable, compute sizes */
    td->n_nullable=0;
    uint32_t minr=0, maxr=0;
    for(int i=0;i<td->n_cols;i++) {
        col_def_t *c=&g_cols[tidx][i];
        if(c->nullable) td->n_nullable++;
        if(c->fixed_len>0) { minr+=c->fixed_len; maxr+=c->fixed_len; }
        else { if(!c->nullable) minr+=1; uint32_t ml=c->max_len; if(ml>16320)ml=16320; maxr+=ml; }
    }
    td->min_rec_len=minr;
    td->max_rec_len=maxr>16320?16320:maxr;

    g_n_tables++;
    fprintf(stderr, "Loaded table '%s': %d cols, %d nullable, min=%u max=%u\n",
        td->table_name, td->n_cols, td->n_nullable,
        td->min_rec_len, td->max_rec_len);
    return tidx;
}

/* ===== Record field decoder ===== */

/* Print escaped string value */
static void print_str(FILE *out, const uint8_t *data, uint32_t len) {
    fputc('"', out);
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        if (c == '"')  { fputc('\\', out); fputc('"', out); }
        else if (c == '\\') { fputc('\\', out); fputc('\\', out); }
        else if (c == '\n') { fputc('\\', out); fputc('n', out); }
        else if (c == '\r') { fputc('\\', out); fputc('r', out); }
        else if (c == '\t') { fputc('\\', out); fputc('t', out); }
        else if (c < 0x20 || c == 0x7f)
            fprintf(out, "\\x%02x", c);
        else
            fputc(c, out);
    }
    fputc('"', out);
}

/* Print hex blob */
static void print_hex(FILE *out, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++)
        fprintf(out, "%02x", data[i]);
}

/* Print InnoDB DATE (3 bytes) */
static void print_date(FILE *out, const uint8_t *d) {
    /* InnoDB DATE: 3 bytes big-endian, value = YYYYMMDD packed
     * Actually stored as: (year*16 + month)*32 + day  in a 3-byte BE int */
    uint32_t v = ((uint32_t)d[0]<<16)|((uint32_t)d[1]<<8)|d[2];
    int day   = v & 0x1f; v >>= 5;
    int month = v & 0x0f; v >>= 4;
    int year  = v;
    fprintf(out, "%04d-%02d-%02d", year, month, day);
}

/* Print InnoDB DATETIME(0): 5 bytes
 * Format: 1 bit sign + 17 bit year_month + 5 bit day + 5 bit hour +
 *         6 bit minute + 6 bit second = 40 bits (5 bytes)
 * MySQL 8.0 DATETIME: stored as a 40-bit big-endian value
 * actual encoding: 40-bit = sign(1) year_month(17) day(5) hour(5) minute(6) second(6)
 * Reference: https://dev.mysql.com/doc/internals/en/date-and-time-data-type-representation.html
 */
static void print_datetime(FILE *out, const uint8_t *d) {
    uint64_t v = ((uint64_t)d[0]<<32)|((uint64_t)d[1]<<24)|
                 ((uint64_t)d[2]<<16)|((uint64_t)d[3]<<8)|d[4];
    /* MySQL 8.0 DATETIME(0) packing (integer part, 40 bits):
     * Bit layout (from MSB): sign(1) yearmonth(17) day(5) hour(5) min(6) sec(6) */
    int sign   = (v >> 39) & 1;
    if (!sign) { fprintf(out, "0000-00-00 00:00:00"); return; }
    uint32_t ym    = (v >> 22) & 0x1FFFF;
    int year  = ym / 13;
    int month = ym % 13;
    int day   = (v >> 17) & 0x1F;
    int hour  = (v >> 12) & 0x1F;
    int min   = (v >>  6) & 0x3F;
    int sec   = v & 0x3F;
    fprintf(out, "%04d-%02d-%02d %02d:%02d:%02d", year,month,day,hour,min,sec);
}

/* Print InnoDB TIMESTAMP(0): 4 bytes big-endian unix timestamp */
static void print_timestamp(FILE *out, const uint8_t *d) {
    uint32_t ts = u32(d);
    if (ts == 0) { fprintf(out,"0000-00-00 00:00:00"); return; }
    time_t t = (time_t)ts;
    struct tm *tm = gmtime(&t);
    if (tm)
        fprintf(out, "%04d-%02d-%02d %02d:%02d:%02d",
                tm->tm_year+1900, tm->tm_mon+1, tm->tm_mday,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    else
        fprintf(out, "%u", ts);
}

/* Print InnoDB TIME(0): 3 bytes
 * Format: sign(1) hour(10) minute(6) second(6) = 23 bits packed big-endian
 */
static void print_time(FILE *out, const uint8_t *d) {
    uint32_t v = ((uint32_t)d[0]<<16)|((uint32_t)d[1]<<8)|d[2];
    int sign  = (v >> 23) & 1;
    int hour  = (v >> 12) & 0x3FF;
    int min   = (v >>  6) & 0x3F;
    int sec   = v & 0x3F;
    fprintf(out, "%s%02d:%02d:%02d", sign?"":"-", hour,min,sec);
}

/* Print InnoDB DECIMAL */
static void print_decimal(FILE *out, const uint8_t *d, int prec, int scale) {
    /* Binary decimal format (same as MySQL 5.7+):
     * Each 9-digit group takes 4 bytes; leftovers take 0..4 bytes */
    static const int dig2bytes[] = {0,1,1,2,2,3,3,4,4,4};
    int int_dig  = prec - scale;
    int frac_dig = scale;

    int int_groups  = int_dig  / 9;
    int frac_groups = frac_dig / 9;
    int int_left    = int_dig  % 9;
    int frac_left   = frac_dig % 9;

    int int_left_bytes  = dig2bytes[int_left];
    int frac_left_bytes = dig2bytes[frac_left];

    const uint8_t *p = d;

    /* Sign is in the high bit of the first byte */
    bool negative = (*p & 0x80) == 0;

    /* Flip sign bit (and if negative, flip all bytes) */
    uint8_t buf[64];
    int total = int_left_bytes + int_groups*4 + frac_groups*4 + frac_left_bytes;
    if (total > 64) total = 64;
    memcpy(buf, d, total);
    buf[0] ^= 0x80;
    if (negative) for (int i = 0; i < total; i++) buf[i] ^= 0xFF;
    p = buf;

    if (negative) fputc('-', out);

    /* Integer part */
    bool first_group = true;
    if (int_left_bytes > 0) {
        uint32_t v = 0;
        for (int i = 0; i < int_left_bytes; i++) v = (v<<8)|*p++;
        if (first_group) { fprintf(out,"%u",v); first_group=false; }
        else fprintf(out,"%09u",v);
    }
    for (int i = 0; i < int_groups; i++) {
        uint32_t v = u32(p); p += 4;
        if (first_group) { fprintf(out,"%u",v); first_group=false; }
        else fprintf(out,"%09u",v);
    }
    if (first_group) fputc('0', out);

    /* Fractional part */
    if (frac_dig > 0) {
        fputc('.', out);
        for (int i = 0; i < frac_groups; i++) {
            uint32_t v = u32(p); p += 4;
            fprintf(out, "%09u", v);
        }
        if (frac_left_bytes > 0) {
            uint32_t v = 0;
            for (int i = 0; i < frac_left_bytes; i++) v=(v<<8)|*p++;
            fprintf(out, "%0*u", frac_left, v);
        }
    }
}

/* Print a single field value */
static void print_field(FILE *out, const uint8_t *data, uint32_t len,
                        const col_def_t *col, bool is_null, bool is_ext) {
    if (is_null) { fprintf(out, "NULL"); return; }

    /* External (off-page) LOB — just print length hint */
    if (is_ext) {
        /* In MySQL 8.0 DYNAMIC format, off-page pointer is 20 bytes:
         * space_id(4) page_no(4) offset(4) length(8).
         * We print the total length as a hint. */
        if (len >= 20) {
            uint64_t blob_len = u64(data + 12);
            fprintf(out, "<BLOB off-page, len=%llu>", (unsigned long long)blob_len);
        } else {
            fprintf(out, "<BLOB off-page, ptr_len=%u>", len);
        }
        return;
    }

    switch (col->type) {
    case COL_TINYINT:
        if (col->unsigned_flag) fprintf(out, "%u", (uint8_t)data[0]);
        else fprintf(out, "%d", (int8_t)(data[0] ^ 0x80));
        break;
    case COL_SMALLINT:
        if (col->unsigned_flag) fprintf(out, "%u", (unsigned)u16(data));
        else fprintf(out, "%d", (int16_t)(u16(data) ^ 0x8000));
        break;
    case COL_MEDIUMINT: {
        uint32_t v = ((uint32_t)data[0]<<16)|((uint32_t)data[1]<<8)|data[2];
        if (col->unsigned_flag) fprintf(out, "%u", v);
        else {
            int32_t sv = (int32_t)(v ^ 0x800000);
            if (sv >= 0x800000) sv -= 0x1000000;  /* sign extend */
            fprintf(out, "%d", sv);
        }
        break;
    }
    case COL_INT:
        if (col->unsigned_flag) fprintf(out, "%u", u32(data));
        else fprintf(out, "%d", (int32_t)(u32(data) ^ 0x80000000UL));
        break;
    case COL_BIGINT:
        if (col->unsigned_flag) fprintf(out, "%llu", (unsigned long long)u64(data));
        else fprintf(out, "%lld", (long long)(u64(data) ^ 0x8000000000000000ULL));
        break;
    case COL_FLOAT: {
        uint32_t bits = u32(data);
        float f; memcpy(&f, &bits, 4);
        fprintf(out, "%g", (double)f);
        break;
    }
    case COL_DOUBLE: {
        uint64_t bits = u64(data);
        double d; memcpy(&d, &bits, 8);
        fprintf(out, "%g", d);
        break;
    }
    case COL_DECIMAL:
        print_decimal(out, data, col->prec, col->scale);
        break;
    case COL_DATE:
        print_date(out, data);
        break;
    case COL_DATETIME:
        print_datetime(out, data);
        break;
    case COL_TIMESTAMP:
        print_timestamp(out, data);
        break;
    case COL_TIME:
        print_time(out, data);
        break;
    case COL_YEAR:
        fprintf(out, "%u", 1900 + data[0]);
        break;
    case COL_BIT: {
        uint64_t v = 0;
        for (uint32_t i = 0; i < len && i < 8; i++) v = (v<<8)|data[i];
        fprintf(out, "%llu", (unsigned long long)v);
        break;
    }
    case COL_CHAR:
    case COL_VARCHAR:
    case COL_BINARY:
    case COL_VARBINARY:
    case COL_ENUM:
    case COL_SET:
        print_str(out, data, len);
        break;
    case COL_TINYBLOB:
    case COL_BLOB:
    case COL_MEDIUMBLOB:
    case COL_LONGBLOB:
    case COL_TINYTEXT:
    case COL_TEXT:
    case COL_MEDIUMTEXT:
    case COL_LONGTEXT:
    case COL_JSON:
        /* Output as hex for binary safety */
        print_str(out, data, len);
        break;
    default:
        print_hex(out, data, len);
        break;
    }
}

/* ===== COMPACT/DYNAMIC record parser ===== */

/*
 * MySQL 8.0 COMPACT record header (from record origin, going backward):
 *   Offset -1: info_bits(4) | n_owned(4)
 *   Offset -2: heap_no(13) | status(3)  -- 2 bytes
 *   Offset -4: next_record_offset (signed 16-bit)  -- 2 bytes
 *   Above that: NULL bitmap (n_nullable/8 bytes, rounded up)
 *   Above that: variable-length field lengths (1 or 2 bytes each)
 *
 * MySQL 8.0 additions vs 5.7:
 *   - If REC_INFO_VERSION_FLAG set: immediately before NULL bitmap is 1 byte version
 *   - If REC_INFO_INSTANT_FLAG set: n_fields encoded in 1 or 2 bytes
 *     before the variable-length array (indicates how many fields are present,
 *     which may be less than the dict's n_fields if columns were instantly dropped)
 *
 * Field length array layout (backward from NULL bitmap):
 *   For each variable-length column (NOT NULL or potentially not null):
 *     If max_len > 255 or column can be >= 256 bytes:
 *       2-byte length: bit 15 = off-page, bit 14-0 = len
 *       If bit 7 set in first byte: 2-byte length
 *     else 1-byte length
 *
 * Returns number of fields parsed (n_fields), or -1 on error.
 * field_data[i], field_len[i], field_null[i], field_ext[i] are filled.
 */

#define MAX_FIELD_LEN  (UNIV_PAGE_SIZE)

static int parse_compact_record(
    const uint8_t *rec,     /* pointer to record origin */
    const uint8_t *page,    /* page start */
    const table_def_t *td,
    const col_def_t *cols,
    const uint8_t **field_data,  /* out: pointer to each field */
    uint32_t       *field_len,   /* out: length of each field */
    bool           *field_null,  /* out: is NULL */
    bool           *field_ext    /* out: is off-page */
) {
    int n = td->n_cols;
    uint8_t info = rec_byte(rec, REC_NEW_INFO_BITS);
    bool has_version = (info & REC_INFO_VERSION_FLAG) != 0;
    bool has_instant = (info & REC_INFO_INSTANT_FLAG) != 0;

    /* How many fields are in this record?
     * MySQL 8.0: if instant, the record may have fewer fields than the dict.
     * The count is encoded just before the variable-length array. */
    int n_rec_fields = n;  /* default: same as table */

    /* Start of extra bytes (above the fixed 5-byte header) */
    /* Layout from rec going backward:
     *  [rec-5] = fixed header (5 bytes)
     *  [rec-6..] = optional version byte (if has_version)
     *  then optional n_fields (if has_instant)
     *  then NULL bitmap
     *  then variable-length array
     */
    const uint8_t *extra = rec - REC_N_NEW_EXTRA_BYTES;  /* = rec - 5 */

    /* Optional: row version (1 byte) */
    if (has_version) extra--;  /* skip version byte, not used for field parsing */

    /* Optional: n_fields encoding (if instant ADD COLUMN was used) */
    if (has_instant) {
        /* Read 1 or 2 byte n_fields.
         * If first byte >= 0x80: 2-byte, n_fields = ((byte0 & 0x7F) << 8) | byte1
         * Else: 1-byte n_fields
         */
        uint8_t b0 = *(extra - 1);
        if (b0 & REC_N_FIELDS_TWO_BYTES_FLAG) {
            uint8_t b1 = *(extra - 2);
            n_rec_fields = ((b0 & 0x7F) << 8) | b1;
            extra -= 2;
        } else {
            n_rec_fields = b0 & 0x7F;
            extra -= 1;
        }
        /* n_rec_fields should be <= n */
        if (n_rec_fields > n) n_rec_fields = n;
    }

    /* NULL bitmap: ceil(n_nullable / 8) bytes, immediately above the extra area */
    int null_bytes = (td->n_nullable + 7) / 8;
    const uint8_t *nulls = extra - 1;  /* points to first (highest addr) null byte */
    const uint8_t *lens  = nulls - null_bytes;  /* variable-length array end */

    /* Current null bit index */
    int null_bit = 0;

    /* Parse field by field */
    const uint8_t *data_ptr = rec;  /* current position in data area */

    for (int i = 0; i < n; i++) {
        const col_def_t *c = &cols[i];

        field_null[i] = false;
        field_ext[i]  = false;
        field_data[i] = NULL;
        field_len[i]  = 0;

        /* Fields beyond n_rec_fields are "instant default" — output NULL/default */
        if (i >= n_rec_fields) {
            field_null[i] = true;
            continue;
        }

        /* Check NULL bit */
        if (c->nullable) {
            int byte_idx = null_bit / 8;
            int bit_idx  = null_bit % 8;
            bool is_null = ((nulls[-byte_idx]) >> bit_idx) & 1;
            null_bit++;
            if (is_null) {
                field_null[i] = true;
                continue;
            }
        }

        /* Fixed length */
        if (c->fixed_len > 0) {
            field_data[i] = data_ptr;
            field_len[i]  = c->fixed_len;
            data_ptr += c->fixed_len;
            continue;
        }

        /* Variable length: read length from lens array */
        uint8_t lb = *lens--;
        uint32_t flen;
        bool ext = false;
        if (c->max_len > 255 && (lb & 0x80)) {
            /* 2-byte length */
            uint8_t lb2 = *lens--;
            ext  = (lb & 0x40) != 0;
            flen = ((uint32_t)(lb & 0x3F) << 8) | lb2;
        } else {
            flen = lb;
        }

        /* Sanity check */
        if (data_ptr + flen > page + UNIV_PAGE_SIZE) {
            if (opt_verbose)
                fprintf(stderr, "  Field %d length %u out of page\n", i, flen);
            return -1;
        }

        field_data[i] = data_ptr;
        field_len[i]  = flen;
        field_ext[i]  = ext;
        data_ptr += flen;
    }

    return n_rec_fields;
}

/* ===== REDUNDANT record parser ===== */

/*
 * Parse a REDUNDANT (old-style) record.
 * Header: REC_N_OLD_EXTRA_BYTES = 6 bytes
 * Before data: field offset array (n_fields entries of 1 or 2 bytes each)
 * Offset from start of array to rec gives absolute positions of field ends.
 */
static int parse_redundant_record(
    const uint8_t *rec,
    const uint8_t *page,
    const table_def_t *td,
    const col_def_t *cols,
    const uint8_t **field_data,
    uint32_t       *field_len,
    bool           *field_null,
    bool           *field_ext
) {
    int n = td->n_cols;

    /* Number of fields from record header */
    uint16_t hdr = u16(rec - REC_OLD_N_FIELDS);
    int n_rec = (hdr & REC_OLD_N_FIELDS_MASK) >> REC_OLD_N_FIELDS_SHIFT;
    if (n_rec > n) n_rec = n;
    if (n_rec <= 0) return -1;

    /* 1-byte vs 2-byte offset array */
    bool short_offs = (rec_byte(rec, REC_OLD_SHORT) & REC_OLD_SHORT_MASK) != 0;
    int  offs_size  = short_offs ? 1 : 2;

    /* Offset array starts just before the extra header */
    const uint8_t *offs_arr = rec - REC_N_OLD_EXTRA_BYTES - offs_size * n_rec;

    for (int i = 0; i < n; i++) {
        field_null[i] = false;
        field_ext[i]  = false;
        field_data[i] = NULL;
        field_len[i]  = 0;

        if (i >= n_rec) {
            field_null[i] = true;
            continue;
        }

        uint32_t offs_val;
        bool is_null, is_ext;
        if (short_offs) {
            uint8_t b = offs_arr[i];
            is_null = (b & REC_1BYTE_SQL_NULL_MASK) != 0;
            is_ext  = false;
            offs_val = b & ~REC_1BYTE_SQL_NULL_MASK;
        } else {
            uint16_t b = u16(offs_arr + i * 2);
            is_null = (b & REC_2BYTE_SQL_NULL_MASK) != 0;
            is_ext  = (b & REC_2BYTE_EXTERN_MASK)   != 0;
            offs_val = b & ~(REC_2BYTE_SQL_NULL_MASK | REC_2BYTE_EXTERN_MASK);
        }

        field_null[i] = is_null;
        field_ext[i]  = is_ext;

        if (is_null) continue;

        /* Previous offset (0 for first field) */
        uint32_t prev_offs = 0;
        if (i > 0) {
            if (short_offs)
                prev_offs = offs_arr[i-1] & ~REC_1BYTE_SQL_NULL_MASK;
            else
                prev_offs = u16(offs_arr + (i-1)*2) &
                            ~(REC_2BYTE_SQL_NULL_MASK|REC_2BYTE_EXTERN_MASK);
        }

        uint32_t flen = offs_val - prev_offs;
        const uint8_t *fptr = rec + prev_offs;

        if (fptr + flen > page + UNIV_PAGE_SIZE) return -1;

        field_data[i] = fptr;
        field_len[i]  = flen;
    }

    return n_rec;
}

/* ===== Page validity check ===== */

/*
 * Walk the record linked list from infimum to supremum.
 * Returns true if the chain terminates at supremum within max_steps.
 */
static bool check_page_valid(const uint8_t *page, bool comp) {
    uint32_t inf = page_get_infimum_offs(comp);
    uint32_t sup = page_get_supremum_offs(comp);
    uint32_t cur = inf;
    int steps = 0;
    int max_steps = UNIV_PAGE_SIZE / 5;

    while (steps++ < max_steps) {
        if (cur < 2 || cur >= UNIV_PAGE_SIZE) return false;
        const uint8_t *rec = page + cur;
        uint32_t next;
        if (comp) {
            int16_t delta = (int16_t)u16(rec - REC_NEXT);
            if (delta == 0) return false;
            next = (uint32_t)((int32_t)cur + delta);
        } else {
            next = u16(rec - REC_NEXT);
        }
        if (next == sup) return true;
        if (next <= inf || next >= UNIV_PAGE_SIZE || next == cur) return false;
        cur = next;
    }
    return false;
}

/* ===== Per-record processing ===== */

static void output_record(const uint8_t *rec, const uint8_t *page,
                          bool comp, const table_def_t *td,
                          const col_def_t *cols) {
    const uint8_t *field_data[MAX_COLS];
    uint32_t       field_len [MAX_COLS];
    bool           field_null[MAX_COLS];
    bool           field_ext [MAX_COLS];

    int n_parsed;
    if (comp)
        n_parsed = parse_compact_record(rec, page, td, cols,
                       field_data, field_len, field_null, field_ext);
    else
        n_parsed = parse_redundant_record(rec, page, td, cols,
                       field_data, field_len, field_null, field_ext);

    if (n_parsed < 0) return;

    /* Quick sanity: total data size */
    uint32_t total_data = 0;
    for (int i = 0; i < td->n_cols; i++) {
        if (!field_null[i] && !field_ext[i])
            total_data += field_len[i];
    }
    if (total_data < td->min_rec_len || total_data > td->max_rec_len + 20) {
        if (opt_verbose)
            fprintf(stderr, "  Skipped record: data_size=%u outside [%u,%u]\n",
                    total_data, td->min_rec_len, td->max_rec_len);
        return;
    }

    /* Write TSV line: table_name TAB field1 TAB field2 ... */
    FILE *out = f_out ? f_out : stdout;
    fprintf(out, "%s", td->table_name);
    for (int i = 0; i < td->n_cols; i++) {
        fputc('\t', out);
        print_field(out, field_data[i], field_len[i],
                    &cols[i], field_null[i], field_ext[i]);
    }
    fputc('\n', out);
    g_recs_dumped++;
}

/* ===== Page processor ===== */

static void process_page(const uint8_t *page, uint64_t page_no) {
    uint16_t page_type = u16(page + FIL_PAGE_TYPE);

    /* We only handle index (B-tree) pages */
    if (page_type != FIL_PAGE_INDEX &&
        page_type != FIL_PAGE_SDI &&
        page_type != FIL_PAGE_RTREE)
        return;

    /* Filter by index_id */
    if (opt_filter_index) {
        uint64_t idx_id = u64(page + PAGE_INDEX_ID);
        if (idx_id != opt_index_id) return;
    }

    g_pages_index++;

    /* Row format detection */
    bool comp;
    if (opt_force_format == 4)      comp = false;
    else if (opt_force_format == 5) comp = true;
    else                             comp = page_is_compact(page);

    uint16_t n_recs   = u16(page + PAGE_N_RECS);
    uint16_t level    = u16(page + PAGE_LEVEL);
    uint16_t garbage  = u16(page + PAGE_GARBAGE);
    uint16_t free_ptr = u16(page + PAGE_FREE);

    /* Only process leaf pages (level == 0) */
    if (level != 0) {
        if (opt_verbose)
            fprintf(stderr, "  Page %llu: non-leaf (level=%u), skipped\n",
                    (unsigned long long)page_no, level);
        return;
    }

    if (opt_skip_nodelpages && (garbage == 0 && free_ptr == 0)) return;

    bool page_valid = check_page_valid(page, comp);

    uint32_t inf = page_get_infimum_offs(comp);
    uint32_t sup = page_get_supremum_offs(comp);

    if (opt_verbose)
        fprintf(stderr,
            "Page %llu: type=%u fmt=%s n_recs=%u valid=%s garbage=%u\n",
            (unsigned long long)page_no, page_type,
            comp?"COMPACT":"REDUNDANT", n_recs,
            page_valid?"yes":"no", garbage);

    g_recs_expected += n_recs;

    /* === Pass 1: walk active record chain === */
    if (page_valid && !opt_deleted_only) {
        uint32_t cur = inf;
        int steps = 0;
        while (steps++ < UNIV_PAGE_SIZE) {
            const uint8_t *rec = page + cur;
            uint32_t next;
            if (comp) {
                int16_t delta = (int16_t)u16(rec - REC_NEXT);
                if (delta == 0) break;
                next = (uint32_t)((int32_t)cur + delta);
            } else {
                next = u16(rec - REC_NEXT);
            }
            if (next == sup) break;
            if (next < 5 || next >= UNIV_PAGE_SIZE) break;

            /* Jump to next record */
            cur = next;
            const uint8_t *nrec = page + cur;

            /* Skip infimum/supremum */
            if (comp) {
                uint8_t status = rec_get_status(nrec);
                if (status == REC_STATUS_INFIMUM || status == REC_STATUS_SUPREMUM) continue;
                if (status != REC_STATUS_ORDINARY) continue;
            }

            bool del = rec_get_deleted_flag(nrec, comp);
            if (del && opt_undeleted_only) continue;
            if (!del && opt_deleted_only)  continue;

            /* Try each table definition */
            for (int t = 0; t < g_n_tables; t++) {
                g_tables[t].is_compact = comp;
                output_record(nrec, page, comp, &g_tables[t], g_cols[t]);
            }
        }
    }

    /* === Pass 2: scan garbage/free list for deleted records === */
    if (free_ptr != 0 && !opt_undeleted_only) {
        uint32_t cur = free_ptr;
        int steps = 0;
        while (steps++ < UNIV_PAGE_SIZE / 5 && cur != 0 && cur < UNIV_PAGE_SIZE) {
            const uint8_t *rec = page + cur;
            bool del = rec_get_deleted_flag(rec, comp);

            /* In free list records are always delete-marked */
            if (del) {
                for (int t = 0; t < g_n_tables; t++) {
                    g_tables[t].is_compact = comp;
                    output_record(rec, page, comp, &g_tables[t], g_cols[t]);
                }
            }

            /* Follow free-list chain */
            uint32_t next;
            if (comp) {
                int16_t delta = (int16_t)u16(rec - REC_NEXT);
                if (delta == 0) break;
                next = (uint32_t)((int32_t)cur + delta);
            } else {
                next = u16(rec - REC_NEXT);
            }
            if (next == 0 || next == cur) break;
            cur = next;
        }
    }

    /* === Pass 3: brute-force scan (damaged page) === */
    if (!page_valid) {
        uint32_t start = comp ? PAGE_NEW_SUPREMUM_END : PAGE_OLD_SUPREMUM_END;
        for (uint32_t off = start; off + 5 < UNIV_PAGE_SIZE; off++) {
            const uint8_t *rec = page + off;
            if (comp) {
                uint8_t status = rec_get_status(rec) & REC_NEW_STATUS_MASK;
                if (status != REC_STATUS_ORDINARY) continue;
            }
            bool del = rec_get_deleted_flag(rec, comp);
            if (del && opt_undeleted_only) continue;
            if (!del && opt_deleted_only)  continue;

            for (int t = 0; t < g_n_tables; t++) {
                g_tables[t].is_compact = comp;
                output_record(rec, page, comp, &g_tables[t], g_cols[t]);
            }
        }
    }
}

/* ===== File/directory scanner ===== */

static void process_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return; }

    uint8_t page[UNIV_PAGE_SIZE];
    uint64_t pno = 0;

    while (fread(page, 1, UNIV_PAGE_SIZE, f) == UNIV_PAGE_SIZE) {
        process_page(page, pno++);
    }
    g_pages_scanned += pno;
    fclose(f);
}

#ifdef _WIN32
static void scan_dir(const char *dir_path) {
    char pattern[2048];
    snprintf(pattern, sizeof(pattern), "%s\\*.page", dir_path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char full[2048];
        snprintf(full, sizeof(full), "%s\\%s", dir_path, fd.cFileName);
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            process_file(full);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}
#else
static void scan_dir(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) { perror(dir_path); return; }
    struct dirent *ent;
    while ((ent = readdir(d))) {
        const char *n = ent->d_name;
        size_t nl = strlen(n);
        if (nl > 5 && strcmp(n + nl - 5, ".page") == 0) {
            char full[2048];
            snprintf(full, sizeof(full), "%s/%s", dir_path, n);
            process_file(full);
        }
    }
    closedir(d);
}
#endif

/* ===== Usage ===== */
static void usage(const char *prog) {
    fprintf(stderr,
        "MySQL 8.0 InnoDB Record Parser\n"
        "Usage: %s -f <path> -t <create.sql> [options]\n\n"
        "Options:\n"
        "  -f <path>    .ibd file or directory of .page files (from stream_parser8)\n"
        "  -t <sql>     CREATE TABLE SQL file\n"
        "  -o <file>    Output TSV file (default: stdout)\n"
        "  -D           Recover deleted records only\n"
        "  -U           Recover undeleted records only (default)\n"
        "  -d           Skip pages with no deleted records (faster)\n"
        "  -v           Verbose\n"
        "  -I <id>      Filter by index_id (hex with 0x prefix or decimal)\n"
        "  -4           Force ROW_FORMAT=REDUNDANT\n"
        "  -5           Force ROW_FORMAT=COMPACT/DYNAMIC\n\n"
        "Output format: TSV with header\n"
        "  tablename TAB col1 TAB col2 ...\n\n"
        "Example:\n"
        "  %s -f /var/lib/mysql/mydb/mytable.ibd -t mytable.sql -o out.tsv\n",
        prog, prog);
}

/* ===== main ===== */
int main(int argc, char *argv[]) {
    char opt_inpath[1024] = "";
    char opt_sqlfil[1024] = "";

    if (argc < 2) { usage(argv[0]); return 1; }

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i],"-f") && i+1<argc)
            snprintf(opt_inpath, sizeof(opt_inpath), "%s", argv[++i]);
        else if (!strcmp(argv[i],"-t") && i+1<argc)
            snprintf(opt_sqlfil, sizeof(opt_sqlfil), "%s", argv[++i]);
        else if (!strcmp(argv[i],"-o") && i+1<argc)
            snprintf(opt_outfile, sizeof(opt_outfile), "%s", argv[++i]);
        else if (!strcmp(argv[i],"-D")) {
            opt_deleted_only = true; opt_undeleted_only = false;
        }
        else if (!strcmp(argv[i],"-U")) {
            opt_undeleted_only = true; opt_deleted_only = false;
        }
        else if (!strcmp(argv[i],"-d"))
            opt_skip_nodel_page = true;
        else if (!strcmp(argv[i],"-v"))
            opt_verbose = true;
        else if (!strcmp(argv[i],"-I") && i+1<argc) {
            opt_filter_index = true;
            char *id_str = argv[++i];
            if (strncmp(id_str,"0x",2)==0 || strncmp(id_str,"0X",2)==0)
                sscanf(id_str+2, "%llx", (unsigned long long *)&opt_index_id);
            else
                sscanf(id_str, "%llu", (unsigned long long *)&opt_index_id);
        }
        else if (!strcmp(argv[i],"-4")) opt_force_format = 4;
        else if (!strcmp(argv[i],"-5")) opt_force_format = 5;
        else if (!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")) {
            usage(argv[0]); return 0;
        }
        else { fprintf(stderr,"Unknown: %s\n",argv[i]); usage(argv[0]); return 1; }
    }

    if (!opt_inpath[0]) { fprintf(stderr,"Error: -f required\n"); return 1; }
    if (!opt_sqlfil[0]) { fprintf(stderr,"Error: -t required\n"); return 1; }

    /* Load table def */
    int tidx = load_create_sql(opt_sqlfil);
    if (tidx < 0) return 1;

    /* Open output */
    if (opt_outfile[0]) {
        f_out = fopen(opt_outfile, "w");
        if (!f_out) { perror(opt_outfile); return 1; }
    }

    /* Write header comment */
    FILE *out = f_out ? f_out : stdout;
    fprintf(out, "-- innodb8-recovery: table=%s cols=%d\n",
            g_tables[0].table_name, g_tables[0].n_cols);
    fprintf(out, "-- format: TSV, one record per line\n");

    /* Check if path is file or directory */
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(opt_inpath);
    bool is_dir = (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY));
#else
    struct stat st;
    bool is_dir = (stat(opt_inpath, &st) == 0 && S_ISDIR(st.st_mode));
#endif

    if (is_dir) {
        /* Recursively scan FIL_PAGE_INDEX subdirectory */
        char subdir[2048];
        snprintf(subdir, sizeof(subdir), "%s/FIL_PAGE_INDEX", opt_inpath);
#ifdef _WIN32
        if (GetFileAttributesA(subdir) != INVALID_FILE_ATTRIBUTES)
            scan_dir(subdir);
        else
            scan_dir(opt_inpath);
#else
        struct stat st2;
        if (stat(subdir, &st2) == 0 && S_ISDIR(st2.st_mode))
            scan_dir(subdir);
        else
            scan_dir(opt_inpath);
#endif
    } else {
        process_file(opt_inpath);
    }

    if (f_out) fclose(f_out);

    fprintf(stderr,
        "\n=== Results ===\n"
        "Pages scanned  : %llu\n"
        "Index pages    : %llu\n"
        "Records expected: %llu\n"
        "Records dumped : %llu\n",
        (unsigned long long)g_pages_scanned,
        (unsigned long long)g_pages_index,
        (unsigned long long)g_recs_expected,
        (unsigned long long)g_recs_dumped);

    return 0;
}
