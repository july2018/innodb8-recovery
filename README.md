# innodb8-recovery

**MySQL 8.0 InnoDB Data Recovery Toolkit**

A port and enhancement of [twindb/undrop-for-innodb](https://github.com/twindb/undrop-for-innodb) for **MySQL 8.0.x**, 
adapted to handle MySQL 8.0's updated InnoDB format:

- `REC_INFO_VERSION_FLAG` — per-row version byte (MySQL 8.0 instant operations)
- `REC_INFO_INSTANT_FLAG` — instant ADD/DROP COLUMN (fewer fields than dict)
- 1-byte or 2-byte `n_fields` encoding in instant records
- `FIL_PAGE_SDI` page type (Serialised Dictionary Information, MySQL 8.0 new)
- DYNAMIC row format as the default (all LOB is off-page)
- CRC32c checksum (vs InnoDB's legacy checksum in older versions)

---

## Tools

| Tool | Purpose |
|------|---------|
| `ibdump8` | Inspect .ibd file: page types, index IDs, record counts |
| `stream_parser8` | Split .ibd file into individual page files by type |
| `create_defs_parser8` | Parse CREATE TABLE SQL → C header for column definitions |
| `c_parser8` | Parse InnoDB pages and extract records to TSV |

---

## Build

### Linux / macOS
```bash
make
```

### Windows (MinGW / Git Bash)
```bash
make windows
```

Dependencies: `gcc`, `make`, `libm` (usually pre-installed).

---

## Recovery Workflow

### Step 1 — Inspect the tablespace
```bash
./ibdump8 -f /var/lib/mysql/mydb/mytable.ibd -v 2>&1 | head -60
```

Outputs:
- File size and page count
- Per-page: type, level, index_id, record count, row format
- Summary table of page types and index IDs

### Step 2 — Split pages by type
```bash
./stream_parser8 -f /var/lib/mysql/mydb/mytable.ibd -o pages-mytable
```

Creates:
```
pages-mytable/
  FIL_PAGE_INDEX/
    0000000003.page
    0000000004.page
    ...
  FIL_PAGE_TYPE_BLOB/
    ...
```

To only extract pages that have deleted records (faster for DROP/DELETE recovery):
```bash
./stream_parser8 -f mytable.ibd -d -o pages-mytable
```

### Step 3 — Prepare CREATE TABLE definition

Save the original CREATE TABLE statement to a file (use `SHOW CREATE TABLE` if you still have access, 
or reconstruct from your application schema):

```sql
-- mytable.sql
CREATE TABLE `mytable` (
  `id`      INT NOT NULL AUTO_INCREMENT,
  `name`    VARCHAR(255) NOT NULL,
  `email`   VARCHAR(255),
  `age`     TINYINT UNSIGNED,
  `score`   DECIMAL(10,2),
  `created` DATETIME,
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

### Step 4 — Recover records
```bash
# Recover all records (including active rows)
./c_parser8 -f pages-mytable -t mytable.sql -o recovered.tsv

# Recover only deleted/dropped records
./c_parser8 -f pages-mytable -t mytable.sql -D -o deleted_records.tsv

# Operate directly on .ibd file (no stream_parser8 needed)
./c_parser8 -f mytable.ibd -t mytable.sql -o recovered.tsv

# Filter by a specific index (use index_id from ibdump8 output)
./c_parser8 -f pages-mytable -t mytable.sql -I 0x159 -o recovered.tsv
```

### Step 5 — Import recovered data
```bash
# Load TSV into a recovery table
mysql mydb_recovery -e "
  LOAD DATA LOCAL INFILE 'recovered.tsv'
  INTO TABLE mytable_recovered
  CHARACTER SET utf8mb4
  FIELDS TERMINATED BY '\t'
  LINES STARTING BY 'mytable\t';"
```

---

## Output Format

The TSV output has one record per line:
```
tablename<TAB>field1<TAB>field2<TAB>...
```

- `NULL` values are printed as `NULL`
- String values are double-quoted with C-style escaping
- Integer, float, decimal values are printed as numbers
- Dates/times are in `YYYY-MM-DD HH:MM:SS` format
- Off-page LOBs (BLOB/TEXT over ~768 bytes) show `<BLOB off-page, len=NNN>`

---

## MySQL 8.0 Specific Notes

### Instant ADD COLUMN (MySQL 8.0.29+)
When columns are added with `ALTER TABLE ... ADD COLUMN ... ALGORITHM=INSTANT`,
existing rows retain the old format. The record header contains:
- `REC_INFO_INSTANT_FLAG` (0x80) set in info bits
- A 1 or 2-byte field count before the NULL bitmap
- Fields beyond the stored count use instant default values

`c_parser8` handles this automatically — missing fields are output as `NULL`.

### Instant DROP COLUMN (MySQL 8.0.29+)  
When columns are dropped with `ALGORITHM=INSTANT`, the column data remains
on the page but the field count is updated. `c_parser8` skips orphaned field data.

### Row Version (MySQL 8.0.29+)
Records modified after an instant operation have `REC_INFO_VERSION_FLAG` (0x40)
set and contain a 1-byte version number. This is handled transparently.

### Page Size
Currently fixed at 16KB (`innodb_page_size=16384`). 
For other sizes (4K/8K/32K/64K), adjust `UNIV_PAGE_SIZE` in `innodb8_types.h`.

---

## Supported Column Types

| MySQL Type | Storage |
|-----------|---------|
| TINYINT | 1 byte |
| SMALLINT | 2 bytes |
| MEDIUMINT | 3 bytes |
| INT/INTEGER | 4 bytes |
| BIGINT | 8 bytes |
| FLOAT | 4 bytes |
| DOUBLE/REAL | 8 bytes |
| DECIMAL(p,s) | Binary coded, p/9 groups × 4 bytes |
| DATE | 3 bytes |
| DATETIME(0) | 5 bytes |
| TIMESTAMP(0) | 4 bytes (Unix epoch) |
| TIME(0) | 3 bytes |
| YEAR | 1 byte |
| CHAR(n) | n bytes (fixed) |
| VARCHAR(n) | 1–2 byte length prefix + data |
| BINARY(n) | n bytes (fixed) |
| VARBINARY(n) | 1–2 byte length prefix + data |
| BLOB/TEXT variants | Variable length (off-page if > ~768B) |
| BIT(n) | ceil(n/8) bytes |
| ENUM | 1–2 bytes |
| SET | 1–8 bytes |
| JSON | Variable (LONGBLOB internally) |

**Note:** Fractional seconds (DATETIME(n), TIMESTAMP(n), TIME(n)) add extra bytes.
Currently only `(0)` precision is fully decoded; fractional part is not printed.

---

## Differences from undrop-for-innodb

| Feature | undrop-for-innodb | innodb8-recovery |
|---------|-------------------|-----------------|
| MySQL 5.x/7.x | ✅ | ❌ |
| MySQL 8.0 | ❌ | ✅ |
| Instant ADD/DROP | ❌ | ✅ |
| Row version byte | ❌ | ✅ |
| SDI page type | ❌ | ✅ |
| CRC32c checksum | ❌ | ✅ |
| DYNAMIC row format | partial | ✅ |
| Windows builds | ❌ | ✅ |

---

## License

MIT License

Based on ideas from [twindb/undrop-for-innodb](https://github.com/twindb/undrop-for-innodb) (Apache 2.0).  
MySQL 8.0 format constants derived from MySQL 8.0.40 source (`storage/innobase/`).
