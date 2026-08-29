.PHONY: all build clean test test-asan bench bench-quick help

.DEFAULT_GOAL := all

BIN      := lolcat-c
ASAN_BIN := build/lolcat-c-asan
SRC      := src/lolcat.c
TABLES   := src/tables.h
GEN      := build/gentables

CC       ?= cc
CSTD     := -std=c11
WARN     := -Wall -Wextra -Wshadow -Wstrict-prototypes
OPT      := -O3 -fomit-frame-pointer
ARCH     := $(shell $(CC) -mcpu=native -E - </dev/null >/dev/null 2>&1 && echo -mcpu=native || echo -march=native)
CFLAGS   ?= $(CSTD) $(WARN) $(OPT) $(ARCH)
LDFLAGS  ?= -pthread

help:
	@echo "Available targets:"
	@echo "  build       - Build ./$(BIN)"
	@echo "  test        - Run the correctness suite"
	@echo "  test-asan   - Run malformed-input tests under AddressSanitizer"
	@echo "  bench       - Full throughput benchmark vs lolcat-ultra and Ruby lolcat"
	@echo "  bench-quick - Throughput benchmark vs lolcat-ultra only"
	@echo "  clean       - Remove build artifacts"

all: build
build: $(BIN)

$(GEN): tools/gentables.c
	@mkdir -p build
	$(CC) $(CSTD) -O2 -ffp-contract=off -o $@ $< -lm

$(TABLES): $(GEN)
	$(GEN) > $@.tmp && mv $@.tmp $@

$(BIN): $(SRC) $(TABLES)
	$(CC) $(CFLAGS) -Isrc -o $@ $(SRC) $(LDFLAGS)

$(ASAN_BIN): $(SRC) $(TABLES)
	@mkdir -p build
	$(CC) $(CSTD) $(WARN) -O1 -g -fsanitize=address -fno-omit-frame-pointer \
		-Isrc -o $@ $(SRC) $(LDFLAGS) -fsanitize=address

test: $(BIN)
	./tests/run_tests.sh

test-asan: $(ASAN_BIN)
	ASAN_OPTIONS=detect_leaks=0 ./tests/malformed.py ./$(ASAN_BIN)

bench: $(BIN)
	./bench/compare.sh

bench-quick: $(BIN)
	QUICK=1 ./bench/compare.sh

clean:
	rm -rf build $(BIN) $(TABLES) target
