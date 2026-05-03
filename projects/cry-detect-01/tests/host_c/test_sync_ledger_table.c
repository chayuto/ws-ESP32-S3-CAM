/* Host-compiled tests for sync_ledger_table.c — the pure helpers extracted
 * from sync_ledger.c. Covers the table data structure (find/upsert/remove)
 * and the JSON walker (used by ledger replay).
 *
 * Build/run:
 *     make test_sync_ledger_table
 *
 * Tests target the in-memory invariants that hardware validation can't
 * easily catch: edge cases in the path-keyed table, malformed ledger
 * lines, replay determinism. */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "sync_ledger.h"
#include "sync_ledger_table.h"

static int g_failures = 0;
static int g_total    = 0;

#define EXPECT(cond, fmt, ...) do {                                 \
    g_total++;                                                       \
    if (!(cond)) {                                                   \
        g_failures++;                                                \
        fprintf(stderr, "FAIL %s:%d: " fmt "\n",                     \
                __FILE__, __LINE__, ##__VA_ARGS__);                  \
    }                                                                \
} while (0)

#define TEST_TABLE_MAX 8

static void make_entry(sync_entry_t *e, const char *path, uint32_t size,
                       uint32_t mtime, sync_state_t state)
{
    memset(e, 0, sizeof(*e));
    strncpy(e->path, path, SYNC_PATH_MAX - 1);
    e->size  = size;
    e->mtime = mtime;
    e->state = state;
    strncpy(e->category, "wav", SYNC_CATEGORY_MAX - 1);
    strncpy(e->sha256_hex, "deadbeefcafebabe", SYNC_SHA256_HEX_LEN);
}

/* ---- Table tests ---- */

static void test_find_index_empty(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    EXPECT(sl_find_index(table, 0, "/anything") == -1, "empty table find");
    EXPECT(sl_find_index(NULL, 0, "/x") == -1, "NULL table find");
    EXPECT(sl_find_index(table, 0, NULL) == -1, "NULL path find");
}

static void test_upsert_insert(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;
    sync_entry_t e;

    make_entry(&e, "/sdcard/a.wav", 100, 1000, SYNC_STATE_PENDING);
    int idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    EXPECT(idx == 0, "first insert returns 0 (got %d)", idx);
    EXPECT(count == 1, "count==1 after first insert (got %d)", count);
    EXPECT(strcmp(table[0].path, "/sdcard/a.wav") == 0, "path stored correctly");
    EXPECT(table[0].size == 100, "size stored correctly");

    make_entry(&e, "/sdcard/b.wav", 200, 2000, SYNC_STATE_PENDING);
    idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    EXPECT(idx == 1, "second insert returns 1 (got %d)", idx);
    EXPECT(count == 2, "count==2 after second insert");
}

static void test_upsert_update_existing(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;
    sync_entry_t e;

    make_entry(&e, "/sdcard/a.wav", 100, 1000, SYNC_STATE_PENDING);
    sl_upsert(table, &count, TEST_TABLE_MAX, &e);

    /* Update same path — same slot, count unchanged. */
    make_entry(&e, "/sdcard/a.wav", 999, 9999, SYNC_STATE_SYNCED);
    int idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    EXPECT(idx == 0, "update returns existing index 0 (got %d)", idx);
    EXPECT(count == 1, "count stays at 1 on update (got %d)", count);
    EXPECT(table[0].size == 999, "size updated");
    EXPECT(table[0].state == SYNC_STATE_SYNCED, "state updated");
}

static void test_upsert_table_full(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;
    sync_entry_t e;
    char path[SYNC_PATH_MAX];

    /* Fill to capacity */
    for (int i = 0; i < TEST_TABLE_MAX; ++i) {
        snprintf(path, sizeof(path), "/sdcard/file%d.wav", i);
        make_entry(&e, path, 100 + i, 1000 + i, SYNC_STATE_PENDING);
        int idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
        EXPECT(idx == i, "fill #%d returned %d", i, idx);
    }
    EXPECT(count == TEST_TABLE_MAX, "count at max");

    /* Overflow on a NEW path → -1 */
    make_entry(&e, "/sdcard/overflow.wav", 999, 9999, SYNC_STATE_PENDING);
    int idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    EXPECT(idx == -1, "overflow returns -1 (got %d)", idx);
    EXPECT(count == TEST_TABLE_MAX, "count unchanged after overflow");

    /* Update of an existing path even when "full" → should succeed */
    make_entry(&e, "/sdcard/file3.wav", 888, 8888, SYNC_STATE_SYNCED);
    idx = sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    EXPECT(idx == 3, "update at full table returns existing index (got %d)", idx);
    EXPECT(table[3].size == 888, "update at full applied");
}

static void test_remove_index_swap_from_tail(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;
    sync_entry_t e;

    /* Insert three files in order */
    for (int i = 0; i < 3; ++i) {
        char p[SYNC_PATH_MAX];
        snprintf(p, sizeof(p), "/sdcard/x%d.wav", i);
        make_entry(&e, p, 100 + i, 1000 + i, SYNC_STATE_PENDING);
        sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    }
    EXPECT(count == 3, "three rows inserted");

    /* Remove middle (index 1) → tail (was at 2) swaps into slot 1 */
    sl_remove_index(table, &count, 1);
    EXPECT(count == 2, "count after remove (got %d)", count);
    EXPECT(strcmp(table[0].path, "/sdcard/x0.wav") == 0, "head preserved");
    EXPECT(strcmp(table[1].path, "/sdcard/x2.wav") == 0,
           "tail swapped into removed slot (got %s)", table[1].path);
}

static void test_remove_index_last(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;
    sync_entry_t e;

    make_entry(&e, "/sdcard/a", 1, 1, SYNC_STATE_PENDING);
    sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    make_entry(&e, "/sdcard/b", 2, 2, SYNC_STATE_PENDING);
    sl_upsert(table, &count, TEST_TABLE_MAX, &e);

    /* Remove the actual last index — no swap should happen, just decrement */
    sl_remove_index(table, &count, 1);
    EXPECT(count == 1, "count==1 after removing tail");
    EXPECT(strcmp(table[0].path, "/sdcard/a") == 0, "head preserved");
}

static void test_remove_index_oob(void)
{
    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 1;
    sync_entry_t e;
    make_entry(&e, "/sdcard/only", 1, 1, SYNC_STATE_PENDING);
    sl_upsert(table, &count, TEST_TABLE_MAX, &e);
    /* sl_upsert sets count=1 again (insert into [0]) — but starting count was 1
     * meaning we're insertin at slot 1. That's still valid; let's not rely on
     * that detail and just check OOB removal is a no-op. */

    int original_count = count;
    sl_remove_index(table, &count, -1);
    EXPECT(count == original_count, "remove negative idx is no-op");
    sl_remove_index(table, &count, 100);
    EXPECT(count == original_count, "remove past-end is no-op");
}

/* ---- JSON walker tests ---- */

static void test_json_extract_str_basic(void)
{
    const char *line = "{\"op\":\"register\",\"path\":\"/sdcard/a.wav\","
                       "\"size\":1234,\"mtime\":56789}";
    char out[64];

    EXPECT(sl_json_extract_str(line, "op", out, sizeof(out)),
           "op extracted");
    EXPECT(strcmp(out, "register") == 0, "op == 'register' (got %s)", out);

    EXPECT(sl_json_extract_str(line, "path", out, sizeof(out)),
           "path extracted");
    EXPECT(strcmp(out, "/sdcard/a.wav") == 0, "path correct (got %s)", out);
}

static void test_json_extract_str_missing_key(void)
{
    const char *line = "{\"op\":\"register\"}";
    char out[64];
    EXPECT(!sl_json_extract_str(line, "missing", out, sizeof(out)),
           "missing key returns false");
}

static void test_json_extract_str_buffer_overflow(void)
{
    const char *line = "{\"path\":\"/sdcard/a-very-long-path-name.wav\"}";
    char out[8];  /* deliberately too small */
    bool ok = sl_json_extract_str(line, "path", out, sizeof(out));
    /* Overflow path: function should still null-terminate; truncation
     * means the closing quote isn't matched, so returns false. */
    EXPECT(!ok, "overflow returns false");
    EXPECT(out[sizeof(out) - 1] == '\0', "out is null-terminated even on overflow");
}

static void test_json_extract_str_value_not_quoted(void)
{
    const char *line = "{\"size\":1234}";
    char out[64];
    EXPECT(!sl_json_extract_str(line, "size", out, sizeof(out)),
           "non-string value rejected by extract_str");
}

static void test_json_extract_uint_basic(void)
{
    const char *line = "{\"size\":1234567,\"mtime\":42}";
    uint32_t v = 0;

    EXPECT(sl_json_extract_uint(line, "size", &v), "size extracted");
    EXPECT(v == 1234567, "size value (got %u)", (unsigned)v);

    EXPECT(sl_json_extract_uint(line, "mtime", &v), "mtime extracted");
    EXPECT(v == 42, "mtime value (got %u)", (unsigned)v);
}

static void test_json_extract_uint_missing_key(void)
{
    const char *line = "{\"size\":1234}";
    uint32_t v = 99;
    EXPECT(!sl_json_extract_uint(line, "absent", &v), "missing key returns false");
    EXPECT(v == 99, "out unchanged on miss");
}

static void test_json_find_key_does_not_match_substring(void)
{
    /* The naive implementation does substring search for "path":
     * make sure it doesn't match e.g. "subpath": as a partial. */
    const char *line = "{\"subpath\":\"X\",\"path\":\"Y\"}";
    char out[16];
    EXPECT(sl_json_extract_str(line, "path", out, sizeof(out)),
           "path extracted from a line with 'subpath'");
    EXPECT(strcmp(out, "Y") == 0, "got value of 'path' not 'subpath' (got %s)", out);
}

static void test_json_extract_str_whitespace_after_colon(void)
{
    /* The walker skips spaces after the colon. */
    const char *line = "{\"path\":   \"/sdcard/x.wav\"}";
    char out[64];
    EXPECT(sl_json_extract_str(line, "path", out, sizeof(out)),
           "path with whitespace after colon");
    EXPECT(strcmp(out, "/sdcard/x.wav") == 0, "value correct");
}

/* ---- Hex encoding ---- */

static void test_hex_encode(void)
{
    uint8_t raw[4] = {0x00, 0xAB, 0xff, 0x10};
    char out[16];
    sl_hex_encode(raw, 4, out);
    EXPECT(strcmp(out, "00abff10") == 0, "hex (got %s)", out);
    EXPECT(out[8] == '\0', "null-terminated");
}

/* ---- Replay-style integration: walks a small ledger fixture ---- */

static void test_replay_simulation(void)
{
    /* Simulate replaying a small ledger by parsing each line and applying
     * to the table the way sync_ledger.c's replay_from_disk() does.
     * Validates that the JSON parser + table ops compose correctly. */
    const char *rows[] = {
        "{\"ts\":1000,\"op\":\"register\",\"path\":\"/sdcard/a.wav\","
        "\"size\":100,\"mtime\":1000,\"sha256\":\"aa\","
        "\"sync_state\":\"pending\",\"category\":\"wav\"}",
        "{\"ts\":1001,\"op\":\"register\",\"path\":\"/sdcard/b.wav\","
        "\"size\":200,\"mtime\":1001,\"sha256\":\"bb\","
        "\"sync_state\":\"pending\",\"category\":\"wav\"}",
        "{\"ts\":1002,\"op\":\"ack\",\"path\":\"/sdcard/a.wav\","
        "\"sha256\":\"aa\",\"sync_state\":\"synced\"}",
        "{\"ts\":1003,\"op\":\"purge\",\"path\":\"/sdcard/b.wav\"}",
        NULL,
    };

    sync_entry_t table[TEST_TABLE_MAX] = {0};
    int count = 0;

    for (int r = 0; rows[r]; ++r) {
        const char *line = rows[r];
        char op[16];
        if (!sl_json_extract_str(line, "op", op, sizeof(op))) continue;
        char path[SYNC_PATH_MAX];
        if (!sl_json_extract_str(line, "path", path, sizeof(path))) continue;

        if (strcmp(op, "purge") == 0) {
            int i = sl_find_index(table, count, path);
            if (i >= 0) sl_remove_index(table, &count, i);
            continue;
        }
        if (strcmp(op, "register") == 0) {
            sync_entry_t e;
            memset(&e, 0, sizeof(e));
            strncpy(e.path, path, SYNC_PATH_MAX - 1);
            sl_json_extract_uint(line, "size",  &e.size);
            sl_json_extract_uint(line, "mtime", &e.mtime);
            sl_json_extract_str(line, "sha256",   e.sha256_hex, sizeof(e.sha256_hex));
            sl_json_extract_str(line, "category", e.category,   sizeof(e.category));
            char st[16] = "pending";
            sl_json_extract_str(line, "sync_state", st, sizeof(st));
            e.state = (strcmp(st, "synced") == 0) ? SYNC_STATE_SYNCED : SYNC_STATE_PENDING;
            sl_upsert(table, &count, TEST_TABLE_MAX, &e);
            continue;
        }
        if (strcmp(op, "ack") == 0) {
            int i = sl_find_index(table, count, path);
            if (i >= 0) table[i].state = SYNC_STATE_SYNCED;
            continue;
        }
    }

    /* Final state: only /sdcard/a.wav, state=synced (b was purged). */
    EXPECT(count == 1, "after replay: 1 row remaining (got %d)", count);
    int i = sl_find_index(table, count, "/sdcard/a.wav");
    EXPECT(i == 0, "a.wav at slot 0");
    EXPECT(table[i].state == SYNC_STATE_SYNCED, "a.wav final state synced");
    EXPECT(table[i].size == 100, "a.wav size preserved");

    /* b.wav should be gone */
    int j = sl_find_index(table, count, "/sdcard/b.wav");
    EXPECT(j == -1, "b.wav purged");
}

/* ---- main ---- */

int main(void)
{
    test_find_index_empty();
    test_upsert_insert();
    test_upsert_update_existing();
    test_upsert_table_full();
    test_remove_index_swap_from_tail();
    test_remove_index_last();
    test_remove_index_oob();

    test_json_extract_str_basic();
    test_json_extract_str_missing_key();
    test_json_extract_str_buffer_overflow();
    test_json_extract_str_value_not_quoted();
    test_json_extract_uint_basic();
    test_json_extract_uint_missing_key();
    test_json_find_key_does_not_match_substring();
    test_json_extract_str_whitespace_after_colon();

    test_hex_encode();

    test_replay_simulation();

    fprintf(stderr, "\nresults: %d/%d passed\n",
            g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
