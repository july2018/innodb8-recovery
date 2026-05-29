#!/bin/bash
# example/recover_orders.sh
# Example: Recover data from deleted orders table
# Adjust paths as needed.

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOL_DIR="$(dirname "$SCRIPT_DIR")"
IBD_FILE="${1:-/var/lib/mysql/mydb/orders.ibd}"
SQL_FILE="$SCRIPT_DIR/orders.sql"
OUT_FILE="$SCRIPT_DIR/orders_recovered.tsv"
PAGES_DIR="$SCRIPT_DIR/pages-orders"

echo "=== innodb8-recovery example ==="
echo "IBD  : $IBD_FILE"
echo "SQL  : $SQL_FILE"
echo "OUT  : $OUT_FILE"
echo

# Step 0: Inspect
echo "--- Step 0: Inspect tablespace ---"
"$TOOL_DIR/ibdump8" -f "$IBD_FILE" || true

# Step 1: Split pages
echo
echo "--- Step 1: Split pages ---"
"$TOOL_DIR/stream_parser8" -f "$IBD_FILE" -o "$PAGES_DIR"

echo "Pages written to: $PAGES_DIR/FIL_PAGE_INDEX/"
ls -la "$PAGES_DIR/FIL_PAGE_INDEX/" | head -10

# Step 2: Parse records
echo
echo "--- Step 2: Parse records ---"
"$TOOL_DIR/c_parser8" \
    -f "$PAGES_DIR" \
    -t "$SQL_FILE"  \
    -o "$OUT_FILE"

echo
echo "=== Recovery complete ==="
echo "Output: $OUT_FILE"
wc -l "$OUT_FILE"

echo
echo "To import:"
echo "  mysql -u root -p mydb_recovery \\"
echo "    -e \"LOAD DATA LOCAL INFILE '$OUT_FILE' INTO TABLE orders \\"
echo "       CHARACTER SET utf8mb4 FIELDS TERMINATED BY '\\\\t' \\"
echo "       LINES STARTING BY 'orders\\\\t';\""
