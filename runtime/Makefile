CC := clang
CFLAGS := -std=c11 -O1 -arch arm64 -Wall -Werror -Iinclude
BUILD := build.nosync
HOBBIT_BIN ?= $(realpath $(shell git rev-parse --git-common-dir)/..)/orig.nosync/hobbit_i386

$(BUILD)/test_loader: tests/test_loader.c src/guest_mem.c src/macho_load.c \
		include/guest_mem.h include/gen_segments.h
	mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tests/test_loader.c src/guest_mem.c src/macho_load.c

.PHONY: test clean
test: $(BUILD)/test_loader
	$(BUILD)/test_loader $(HOBBIT_BIN)

clean:
	rm -rf $(BUILD)
