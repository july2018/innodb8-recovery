/*
 * create_defs_parser.c - Parse MySQL CREATE TABLE statement to generate
 *   table column definitions used by c_parser8.
 *
 * Usage:
 *   create_defs_parser8 < create.sql > table_defs.h
 *
 * Reads a single CREATE TABLE statement from stdin and outputs a C
 * header (table_defs.h) that c_parser8.c #includes.
 *
 * Supports column types:
 *   TINYINT, SMALLINT, MEDIUMINT, INT/INTEGER, BIGINT,
 *   FLOAT, DOUBLE, DECIMAL/NUMERIC,
 *   DATE, DATETIME, TIMESTAMP, TIME, YEAR,
 *   CHAR, VARCHAR, BINARY, VARBINARY,
 *   TINYBLOB, BLOB, MEDIUMBLOB, LONGBLOB,
 *   TINYTEXT, TEXT, MEDIUMTEXT, LONGTEXT,
 *   BIT, ENUM, SET, JSON
 *
 * MySQL 8.0: InnoDB physical storage sizes are the same as MySQL 5.7
 * for fixed-length types.  Variable-length (VARCHAR/BLOB/TEXT) are
 * stored in DYNAMIC format by default in MySQL 8.0.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>

#include "innodb8_types.h"

#define MAX_SQL_LEN   65536
#define MAX_ENUM_VALS 512

/* ===== string utilities ===== */
static char *str_trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s) - 1;
    while (e >= s && isspace((unsigned char)*e)) *e-- = '\0';
    return s;
}

static char *str_upper(char *s) {
    for (char *p = s; *p; p++) *p = (char)toupper((unsigned char)*p);
    return s;
}

/* Strip backticks and quotes from identifier */
static char *strip_quotes(char *s) {
    char *src = s, *dst = s;
    while (*src) {
        if (*src != '`' && *src != '"' && *src != '\'')
            *dst++ = *src;
        src++;
    }
    *dst = '\0';
    return s;
}

/* ===== type resolution ===== */
typedef struct {
    const char *name;
    col_type_t  type;
    int         fixed_len;  /* -1 = variable */
} type_map_t;

static const type_map_t type_map[] = {
    { "TINYINT",    COL_TINYINT,    1 },
    { "SMALLINT",   COL_SMALLINT,   2 },
    { "MEDIUMINT",  COL_MEDIUMINT,  3 },
    { "INTEGER",    COL_INT,        4 },
    { "INT",        COL_INT,        4 },
    { "BIGINT",     COL_BIGINT,     8 },
    { "FLOAT",      COL_FLOAT,      4 },
    { "DOUBLE",     COL_DOUBLE,     8 },
    { "REAL",       COL_DOUBLE,     8 },
    { "DECIMAL",    COL_DECIMAL,   -1 },
    { "NUMERIC",    COL_DECIMAL,   -1 },
    { "DATE",       COL_DATE,       3 },
    { "DATETIME",   COL_DATETIME,   5 },  /* MySQL 8.0 fractional: 5+extra */
    { "TIMESTAMP",  COL_TIMESTAMP,  4 },  /* 4 bytes + optional frac */
    { "TIME",       COL_TIME,       3 },
    { "YEAR",       COL_YEAR,       1 },
    { "CHAR",       COL_CHAR,      -1 },  /* fixed, but length param needed */
    { "VARCHAR",    COL_VARCHAR,   -1 },
    { "BINARY",     COL_BINARY,    -1 },
    { "VARBINARY",  COL_VARBINARY, -1 },
    { "TINYBLOB",   COL_TINYBLOB,  -1 },
    { "TINYTEXT",   COL_TINYTEXT,  -1 },
    { "BLOB",       COL_BLOB,      -1 },
    { "TEXT",       COL_TEXT,      -1 },
    { "MEDIUMBLOB", COL_MEDIUMBLOB,-1 },
    { "MEDIUMTEXT", COL_MEDIUMTEXT,-1 },
    { "LONGBLOB",   COL_LONGBLOB,  -1 },
    { "LONGTEXT",   COL_LONGTEXT,  -1 },
    { "BIT",        COL_BIT,       -1 },
    { "ENUM",       COL_ENUM,      -1 },
    { "SET",        COL_SET,       -1 },
    { "JSON",       COL_JSON,      -1 },
    { NULL, COL_UNKNOWN, 0 }
};

static col_type_t lookup_type(const char *name) {
    for (int i = 0; type_map[i].name; i++) {
        if (strcasecmp(name, type_map[i].name) == 0)
            return type_map[i].type;
    }
    return COL_UNKNOWN;
}

static int lookup_fixed_len(col_type_t type) {
    for (int i = 0; type_map[i].name; i++) {
        if (type_map[i].type == type)
            return type_map[i].fixed_len;
    }
    return -1;
}

/* ===== column parser ===== */

/*
 * Parse: `colname` TYPE[(len[,scale])] [UNSIGNED] [NOT NULL] [DEFAULT ...]
 * Returns 1 on success, 0 to skip (constraint line, KEY, etc.)
 */
static int parse_column(char *line, col_def_t *col) {
    memset(col, 0, sizeof(*col));
    col->nullable = true;

    char *p = str_trim(line);

    /* Skip table/index constraints: PRIMARY KEY, KEY, INDEX, UNIQUE, ... */
    if (strncasecmp(p, "PRIMARY", 7) == 0 ||
        strncasecmp(p, "KEY",     3) == 0 ||
        strncasecmp(p, "INDEX",   5) == 0 ||
        strncasecmp(p, "UNIQUE",  6) == 0 ||
        strncasecmp(p, "CONSTRAINT", 10) == 0 ||
        strncasecmp(p, "FULLTEXT", 8) == 0 ||
        strncasecmp(p, "SPATIAL", 7) == 0 ||
        strncasecmp(p, "CHECK",   5) == 0)
        return 0;

    /* column name: may be backtick-quoted */
    char colname[MAX_COL_NAME] = "";
    if (*p == '`' || *p == '"') {
        char q = *p++;
        char *end = strchr(p, q);
        if (!end) return 0;
        int len = (int)(end - p);
        if (len >= MAX_COL_NAME) len = MAX_COL_NAME - 1;
        strncpy(colname, p, len);
        colname[len] = '\0';
        p = end + 1;
    } else {
        /* unquoted */
        char *ws = p;
        while (*ws && !isspace((unsigned char)*ws)) ws++;
        int len = (int)(ws - p);
        if (len >= MAX_COL_NAME) len = MAX_COL_NAME - 1;
        strncpy(colname, p, len);
        colname[len] = '\0';
        p = ws;
    }
    if (!colname[0]) return 0;
    snprintf(col->name, sizeof(col->name), "%s", colname);

    p = str_trim(p);

    /* Type name */
    char typename_buf[64] = "";
    char *ts = p;
    while (*ts && *ts != '(' && !isspace((unsigned char)*ts)) ts++;
    int tlen = (int)(ts - p);
    if (tlen <= 0 || tlen >= (int)sizeof(typename_buf)) return 0;
    strncpy(typename_buf, p, tlen);
    typename_buf[tlen] = '\0';
    str_upper(typename_buf);
    p = ts;

    col->type = lookup_type(typename_buf);
    if (col->type == COL_UNKNOWN) {
        /* might be a known type we don't handle — skip silently */
        return 0;
    }

    /* Optional (len) or (len,scale) */
    int param1 = 0, param2 = 0;
    bool has_param = false;
    p = str_trim(p);
    if (*p == '(') {
        p++;
        has_param = true;
        param1 = atoi(p);
        while (*p && *p != ',' && *p != ')') p++;
        if (*p == ',') {
            p++;
            param2 = atoi(p);
            while (*p && *p != ')') p++;
        }
        if (*p == ')') p++;
    }

    /* UNSIGNED */
    char rest[512];
    snprintf(rest, sizeof(rest), "%s", p);
    str_upper(rest);
    if (strstr(rest, "UNSIGNED")) col->unsigned_flag = true;
    /* NOT NULL */
    if (strstr(rest, "NOT NULL")) col->nullable = false;

    /* Compute fixed_len / max_len */
    switch (col->type) {
    case COL_TINYINT:   col->fixed_len = 1; break;
    case COL_SMALLINT:  col->fixed_len = 2; break;
    case COL_MEDIUMINT: col->fixed_len = 3; break;
    case COL_INT:       col->fixed_len = 4; break;
    case COL_BIGINT:    col->fixed_len = 8; break;
    case COL_FLOAT:     col->fixed_len = 4; break;
    case COL_DOUBLE:    col->fixed_len = 8; break;
    case COL_DATE:      col->fixed_len = 3; break;
    case COL_YEAR:      col->fixed_len = 1; break;
    case COL_TIME:      col->fixed_len = 3; break;    /* TIME(0) */
    case COL_TIMESTAMP: col->fixed_len = 4; break;    /* TIMESTAMP(0) */
    case COL_DATETIME:  col->fixed_len = 5; break;    /* DATETIME(0) */

    case COL_DECIMAL: {
        /* InnoDB stores DECIMAL in binary: ceil(p/9)*4 bytes */
        int prec  = has_param ? param1 : 10;
        int scale = has_param ? param2 : 0;
        col->prec  = prec;
        col->scale = scale;
        int int_part  = prec - scale;
        int int_bytes  = (int_part / 9) * 4 + ((int_part  % 9 + 1) / 2);
        int frac_bytes = (scale   / 9) * 4 + ((scale      % 9 + 1) / 2);
        col->fixed_len = int_bytes + frac_bytes;
        break;
    }

    case COL_BIT: {
        /* ceil(n/8) bytes in InnoDB */
        int bits = has_param ? param1 : 1;
        col->fixed_len = (bits + 7) / 8;
        break;
    }

    case COL_CHAR:
    case COL_BINARY:
        /* Fixed length in bytes (charset may multiply, but we use declared bytes) */
        col->fixed_len = has_param ? param1 : 1;
        col->max_len   = col->fixed_len;
        break;

    case COL_VARCHAR:
    case COL_VARBINARY:
        col->fixed_len = 0;   /* variable */
        col->max_len   = has_param ? param1 : 255;
        break;

    case COL_TINYBLOB:
    case COL_TINYTEXT:
        col->fixed_len = 0;
        col->max_len   = 255;
        break;

    case COL_BLOB:
    case COL_TEXT:
        col->fixed_len = 0;
        col->max_len   = 65535;
        break;

    case COL_MEDIUMBLOB:
    case COL_MEDIUMTEXT:
        col->fixed_len = 0;
        col->max_len   = 16777215;
        break;

    case COL_LONGBLOB:
    case COL_LONGTEXT:
        col->fixed_len = 0;
        col->max_len   = 4294967295UL;
        break;

    case COL_ENUM: {
        /* ENUM storage: 1 or 2 bytes depending on value count.
         * We default to 2 bytes (safe upper bound). */
        col->fixed_len = 2;
        col->max_len   = 2;
        break;
    }
    case COL_SET: {
        /* SET storage: 1-8 bytes. Default 8 bytes (safe). */
        col->fixed_len = 8;
        col->max_len   = 8;
        break;
    }
    case COL_JSON:
        col->fixed_len = 0;
        col->max_len   = 4294967295UL;
        break;

    default:
        col->fixed_len = 0;
        col->max_len   = 65535;
        break;
    }

    return 1;
}

/* ===== table_def output ===== */

static const char *col_type_cname(col_type_t t) {
    switch (t) {
    case COL_TINYINT:   return "COL_TINYINT";
    case COL_SMALLINT:  return "COL_SMALLINT";
    case COL_MEDIUMINT: return "COL_MEDIUMINT";
    case COL_INT:       return "COL_INT";
    case COL_BIGINT:    return "COL_BIGINT";
    case COL_FLOAT:     return "COL_FLOAT";
    case COL_DOUBLE:    return "COL_DOUBLE";
    case COL_DECIMAL:   return "COL_DECIMAL";
    case COL_DATE:      return "COL_DATE";
    case COL_DATETIME:  return "COL_DATETIME";
    case COL_TIMESTAMP: return "COL_TIMESTAMP";
    case COL_TIME:      return "COL_TIME";
    case COL_YEAR:      return "COL_YEAR";
    case COL_CHAR:      return "COL_CHAR";
    case COL_VARCHAR:   return "COL_VARCHAR";
    case COL_BINARY:    return "COL_BINARY";
    case COL_VARBINARY: return "COL_VARBINARY";
    case COL_TINYBLOB:  return "COL_TINYBLOB";
    case COL_BLOB:      return "COL_BLOB";
    case COL_MEDIUMBLOB:return "COL_MEDIUMBLOB";
    case COL_LONGBLOB:  return "COL_LONGBLOB";
    case COL_TINYTEXT:  return "COL_TINYTEXT";
    case COL_TEXT:      return "COL_TEXT";
    case COL_MEDIUMTEXT:return "COL_MEDIUMTEXT";
    case COL_LONGTEXT:  return "COL_LONGTEXT";
    case COL_BIT:       return "COL_BIT";
    case COL_ENUM:      return "COL_ENUM";
    case COL_SET:       return "COL_SET";
    case COL_JSON:      return "COL_JSON";
    default:            return "COL_UNKNOWN";
    }
}

/* ===== main: read SQL from stdin, write table_defs.h to stdout ===== */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    char sql[MAX_SQL_LEN];
    char table_name[MAX_COL_NAME] = "unknown";
    col_def_t cols[MAX_COLS];
    int n_cols = 0;

    /* Read all stdin into buffer */
    char line[4096];
    size_t pos = 0;
    while (fgets(line, sizeof(line), stdin) && pos < MAX_SQL_LEN - 1) {
        size_t ll = strlen(line);
        if (pos + ll < MAX_SQL_LEN) {
            memcpy(sql + pos, line, ll);
            pos += ll;
        }
    }
    sql[pos] = '\0';

    /* Extract table name: CREATE TABLE [IF NOT EXISTS] `name` */
    char *tstart = sql;
    char *found  = NULL;
    /* case-insensitive search for CREATE */
    char sqlup[MAX_SQL_LEN];
    strncpy(sqlup, sql, MAX_SQL_LEN - 1);
    for (int i = 0; sqlup[i]; i++)
        sqlup[i] = (char)toupper((unsigned char)sqlup[i]);

    char *ct = strstr(sqlup, "CREATE");
    if (!ct) { fprintf(stderr, "No CREATE TABLE found\n"); return 1; }
    char *tp = strstr(ct, "TABLE");
    if (!tp) { fprintf(stderr, "No TABLE keyword\n"); return 1; }

    /* Advance to table name position */
    char *real_tp = sql + (tp - sqlup) + 5;  /* skip TABLE */
    real_tp = str_trim(real_tp);

    /* Skip optional IF NOT EXISTS */
    if (strncasecmp(real_tp, "IF", 2) == 0) {
        real_tp += 2;
        real_tp = str_trim(real_tp);
        if (strncasecmp(real_tp, "NOT", 3) == 0) real_tp += 3;
        real_tp = str_trim(real_tp);
        if (strncasecmp(real_tp, "EXISTS", 6) == 0) real_tp += 6;
        real_tp = str_trim(real_tp);
    }

    /* Parse table name (quoted or unquoted) */
    if (*real_tp == '`' || *real_tp == '"') {
        char q = *real_tp++;
        char *tend = strchr(real_tp, q);
        if (tend) {
            int tl = (int)(tend - real_tp);
            if (tl >= MAX_COL_NAME) tl = MAX_COL_NAME - 1;
            strncpy(table_name, real_tp, tl);
            table_name[tl] = '\0';
        }
    } else {
        char *tend = real_tp;
        while (*tend && *tend != '(' && !isspace((unsigned char)*tend)) tend++;
        int tl = (int)(tend - real_tp);
        if (tl >= MAX_COL_NAME) tl = MAX_COL_NAME - 1;
        strncpy(table_name, real_tp, tl);
        table_name[tl] = '\0';
    }
    /* strip database prefix db.table */
    char *dot = strchr(table_name, '.');
    if (dot) memmove(table_name, dot + 1, strlen(dot));

    /* Find opening paren of column list */
    char *colstart = strchr(sql + (tp - sqlup), '(');
    if (!colstart) { fprintf(stderr, "Cannot find column list\n"); return 1; }
    colstart++;

    /* Split by commas, respecting nested parens */
    char colbuf[MAX_SQL_LEN];
    strncpy(colbuf, colstart, sizeof(colbuf) - 1);

    char *p = colbuf;
    int   depth = 1;
    char  colline[2048];
    int   ci = 0;

    while (*p && depth > 0) {
        if (*p == '(') depth++;
        else if (*p == ')') {
            depth--;
            if (depth == 0) break;
        }

        if (*p == ',' && depth == 1) {
            /* process collected colline */
            colline[ci] = '\0';
            char *trimmed = str_trim(colline);
            if (*trimmed) {
                col_def_t c;
                if (parse_column(trimmed, &c) && n_cols < MAX_COLS)
                    cols[n_cols++] = c;
            }
            ci = 0;
        } else {
            if (ci < (int)sizeof(colline) - 1)
                colline[ci++] = *p;
        }
        p++;
    }
    /* last column before ')' */
    if (ci > 0) {
        colline[ci] = '\0';
        char *trimmed = str_trim(colline);
        if (*trimmed) {
            col_def_t c;
            if (parse_column(trimmed, &c) && n_cols < MAX_COLS)
                cols[n_cols++] = c;
        }
    }

    if (n_cols == 0) {
        fprintf(stderr, "No columns parsed!\n");
        return 1;
    }

    /* Count nullable */
    int n_nullable = 0;
    for (int i = 0; i < n_cols; i++)
        if (cols[i].nullable) n_nullable++;

    /* Compute min/max record data size */
    uint32_t min_rec = 0, max_rec = 0;
    for (int i = 0; i < n_cols; i++) {
        if (cols[i].fixed_len > 0) {
            min_rec += cols[i].fixed_len;
            max_rec += cols[i].fixed_len;
        } else {
            /* variable: min=0 (nullable or empty), max=max_len (capped at page) */
            if (!cols[i].nullable) min_rec += 1;  /* at least 1 byte */
            uint32_t ml = cols[i].max_len;
            if (ml > 16320) ml = 16320;  /* on-page cap */
            max_rec += ml;
        }
    }

    /* Emit table_defs.h */
    printf("/*\n");
    printf(" * table_defs.h - auto-generated by create_defs_parser8\n");
    printf(" * Table: %s  Columns: %d  Nullable: %d\n",
           table_name, n_cols, n_nullable);
    printf(" */\n\n");
    printf("#ifndef TABLE_DEFS_H\n");
    printf("#define TABLE_DEFS_H\n\n");
    printf("#include \"innodb8_types.h\"\n\n");

    printf("static col_def_t %s_cols[] = {\n", table_name);
    for (int i = 0; i < n_cols; i++) {
        col_def_t *c = &cols[i];
        printf("    { \"%s\", %s, %u, %u, %s, %s, %d, %d },\n",
               c->name,
               col_type_cname(c->type),
               c->fixed_len,
               c->max_len,
               c->nullable ? "true" : "false",
               c->unsigned_flag ? "true" : "false",
               c->prec, c->scale);
    }
    printf("};\n\n");

    printf("static table_def_t table_definitions[] = {\n");
    printf("    {\n");
    printf("        \"%s\",\n", table_name);
    printf("        /* cols ptr set at runtime */\n");
    printf("        %d, /* n_cols */\n", n_cols);
    printf("        %d, /* n_nullable */\n", n_nullable);
    printf("        %u, /* min_rec_len */\n", min_rec);
    printf("        %u, /* max_rec_len */\n", max_rec > 16320 ? 16320 : max_rec);
    printf("        false /* is_compact: override at runtime */\n");
    printf("    }\n");
    printf("};\n");
    printf("#define N_TABLE_DEFS 1\n\n");
    printf("#define TABLE_COLS_INIT() do { \\\n");
    printf("    table_definitions[0].cols[0] = %s_cols[0]; /* copy */ \\\n",
           table_name);
    printf("} while(0)\n\n");
    printf("#endif /* TABLE_DEFS_H */\n");

    fprintf(stderr,
        "Parsed table '%s': %d columns, %d nullable, "
        "min_rec=%u max_rec=%u\n",
        table_name, n_cols, n_nullable, min_rec,
        max_rec > 16320 ? 16320 : max_rec);

    return 0;
}
