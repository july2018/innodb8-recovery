CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -g
LDFLAGS ?= -lm

HEADERS = innodb8_types.h

TARGETS = stream_parser8 \
          c_parser8       \
          create_defs_parser8 \
          ibdump8

.PHONY: all clean

all: $(TARGETS)

stream_parser8: stream_parser8.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

c_parser8: c_parser8.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

create_defs_parser8: create_defs_parser8.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

ibdump8: ibdump8.c $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGETS) *.o

# Windows (MinGW)
.PHONY: windows
windows:
	$(CC) $(CFLAGS) -o stream_parser8.exe stream_parser8.c $(LDFLAGS)
	$(CC) $(CFLAGS) -o c_parser8.exe       c_parser8.c       $(LDFLAGS)
	$(CC) $(CFLAGS) -o create_defs_parser8.exe create_defs_parser8.c $(LDFLAGS)
	$(CC) $(CFLAGS) -o ibdump8.exe         ibdump8.c         $(LDFLAGS)

# Quick build + test
.PHONY: test
test: all
	@echo "=== Build OK ==="
	@./stream_parser8 --help 2>&1 | head -3 || true
	@./c_parser8 --help 2>&1 | head -3 || true
	@./ibdump8 --help 2>&1 | head -3 || true
