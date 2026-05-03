# Host-compiled C unit tests for cry-detect-01.
#
# Targets pure-logic modules under main/ that have no IDF runtime
# dependencies (FreeRTOS, esp_log, mbedtls, etc.). These modules were
# carved out of larger files specifically so they can be exercised here
# without stubbing.
#
# Usage:
#     make           # build + run every test
#     make build     # compile only
#     make clean
#     make test_parse_range  # run a single test
#
# Cycle time is sub-second; integrate with `make` after every edit.

CC      ?= cc
# Skip -Wpedantic so we can keep ##__VA_ARGS__ in EXPECT() macros.
# Test code, host-only — strictness adds friction without catching real bugs.
CFLAGS  ?= -Wall -Wextra -Werror -O0 -g -std=c11
INCLUDES = -Istubs -I../../main

BUILD_DIR = out
TESTS = test_parse_range test_sync_ledger_table

.PHONY: all build clean run

all: build run

build: $(addprefix $(BUILD_DIR)/, $(TESTS))

run: build
	@echo "=== running tests ==="
	@for t in $(TESTS); do \
		echo ""; echo "--- $$t ---"; \
		./$(BUILD_DIR)/$$t || exit $$?; \
	done
	@echo ""
	@echo "=== all tests passed ==="

# test_parse_range: pure parser + the test driver.
$(BUILD_DIR)/test_parse_range: test_parse_range.c ../../main/range_parser.c ../../main/range_parser.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_parse_range.c ../../main/range_parser.c

# test_sync_ledger_table: pure helpers from sync_ledger_table.c. Pulls in
# sync_ledger.h for the sync_entry_t typedef but no runtime IDF code.
$(BUILD_DIR)/test_sync_ledger_table: test_sync_ledger_table.c ../../main/sync_ledger_table.c ../../main/sync_ledger_table.h ../../main/sync_ledger.h | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_sync_ledger_table.c ../../main/sync_ledger_table.c

$(BUILD_DIR):
	mkdir -p $@

clean:
	rm -rf $(BUILD_DIR)
