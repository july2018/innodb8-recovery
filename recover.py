#!/usr/bin/env python3
"""
recover.py - High-level recovery helper script for innodb8-recovery
Wraps stream_parser8 + c_parser8 in a single command.

Usage:
  python3 recover.py --ibd <file.ibd> --sql <create.sql> [--out <out.tsv>]
                     [--deleted] [--verbose]
"""

import argparse
import os
import subprocess
import sys
import platform

def find_tool(name):
    """Find compiled binary in same directory as this script."""
    base = os.path.dirname(os.path.abspath(__file__))
    candidates = [
        os.path.join(base, name),
        os.path.join(base, name + '.exe'),
        name,  # in PATH
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return None

def run(cmd, check=True):
    print(f"[CMD] {' '.join(str(x) for x in cmd)}", file=sys.stderr)
    result = subprocess.run(cmd, check=check)
    return result.returncode

def main():
    p = argparse.ArgumentParser(
        description='MySQL 8.0 InnoDB recovery helper')
    p.add_argument('--ibd',    required=True, help='InnoDB .ibd file')
    p.add_argument('--sql',    required=True, help='CREATE TABLE SQL file')
    p.add_argument('--out',    default='recovered.tsv', help='Output TSV file')
    p.add_argument('--deleted', action='store_true',
                   help='Recover only deleted records')
    p.add_argument('--index-id', help='Filter by index_id (hex or decimal)')
    p.add_argument('--skip-split', action='store_true',
                   help='Skip stream_parser8 step (pages already split)')
    p.add_argument('--pages-dir', help='Directory of split pages (for --skip-split)')
    p.add_argument('--verbose', '-v', action='store_true')
    p.add_argument('--force-format', choices=['4','5'],
                   help='4=REDUNDANT 5=COMPACT/DYNAMIC')
    args = p.parse_args()

    ibd = args.ibd
    if not os.path.exists(ibd):
        print(f"Error: {ibd} not found", file=sys.stderr)
        sys.exit(1)

    sql_file = args.sql
    if not os.path.exists(sql_file):
        print(f"Error: {sql_file} not found", file=sys.stderr)
        sys.exit(1)

    # Find tools
    stream = find_tool('stream_parser8')
    parser = find_tool('c_parser8')
    dumper = find_tool('ibdump8')

    if not parser:
        print("Error: c_parser8 not found. Run 'make' first.", file=sys.stderr)
        sys.exit(1)

    # Step 0: Dump info
    if dumper:
        print(f"\n=== Step 0: Inspect {ibd} ===", file=sys.stderr)
        run([dumper, '-f', ibd], check=False)

    # Step 1: Split pages
    ibd_base = os.path.basename(ibd)
    pages_dir = args.pages_dir if args.pages_dir else f'pages-{ibd_base}'

    if not args.skip_split:
        if not stream:
            print("Warning: stream_parser8 not found; running c_parser8 directly on .ibd",
                  file=sys.stderr)
            pages_dir = None
        else:
            print(f"\n=== Step 1: Split pages → {pages_dir} ===", file=sys.stderr)
            cmd = [stream, '-f', ibd, '-o', pages_dir]
            if args.deleted: cmd.append('-d')
            if args.verbose: cmd.append('-v')
            run(cmd, check=False)
    else:
        if not args.pages_dir:
            print("Error: --skip-split requires --pages-dir", file=sys.stderr)
            sys.exit(1)

    # Step 2: Parse records
    print(f"\n=== Step 2: Parse records → {args.out} ===", file=sys.stderr)
    source = pages_dir if pages_dir and os.path.isdir(pages_dir) else ibd
    cmd = [parser, '-f', source, '-t', sql_file, '-o', args.out]
    if args.deleted:  cmd.append('-D')
    if args.verbose:  cmd.append('-v')
    if args.index_id: cmd += ['-I', args.index_id]
    if args.force_format: cmd.append(f'-{args.force_format}')
    run(cmd)

    print(f"\n=== Done ===", file=sys.stderr)
    print(f"Output: {args.out}", file=sys.stderr)

    # Quick stats
    if os.path.exists(args.out):
        with open(args.out, 'r', encoding='utf-8', errors='replace') as f:
            lines = [l for l in f if not l.startswith('--')]
        print(f"Records recovered: {len(lines)}", file=sys.stderr)

    print(f"""
To load into MySQL:
  mysql -u root -p mydb_recovery <<'EOF'
  LOAD DATA LOCAL INFILE '{args.out}'
  INTO TABLE <your_table>
  CHARACTER SET utf8mb4
  FIELDS TERMINATED BY '\\t'
  LINES STARTING BY '<tablename>\\t';
  EOF
""", file=sys.stderr)

if __name__ == '__main__':
    main()
