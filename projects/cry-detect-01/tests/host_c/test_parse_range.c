/* Host-compiled tests for range_parser.c — the pure HTTP Range parser
 * extracted from file_api.c. Each case calls range_parse_value with a
 * candidate header value and asserts the (rc, start, end) tuple.
 *
 * Build/run via the Makefile:
 *     make test_parse_range
 *
 * Subset coverage rationale: the parser feeds Phase B's resume path. A
 * single off-by-one or missing-input case here corrupts every partial
 * download silently, so we cover boundary conditions densely. */

#include <stdio.h>
#include <string.h>
#include "range_parser.h"

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

static void check(const char *label, const char *hdr, long file_size,
                  int want_rc, long want_start, long want_end)
{
    long s = -7777, e = -7777;
    int rc = range_parse_value(hdr, file_size, &s, &e);
    EXPECT(rc == want_rc, "[%s] rc: got %d want %d (hdr=%s, size=%ld)",
           label, rc, want_rc, hdr ? hdr : "(null)", file_size);
    if (want_rc == 1 && rc == 1) {
        EXPECT(s == want_start, "[%s] start: got %ld want %ld", label, s, want_start);
        EXPECT(e == want_end,   "[%s] end:   got %ld want %ld", label, e, want_end);
    }
}

int main(void)
{
    /* Positive cases */
    check("simple closed range",   "bytes=0-99",      1000, 1, 0,    99);
    check("single byte at start",  "bytes=0-0",       1000, 1, 0,    0);
    check("single byte mid-file",  "bytes=500-500",   1000, 1, 500,  500);
    check("open range to EOF",     "bytes=100-",      1000, 1, 100,  999);
    check("from zero to EOF",      "bytes=0-",        1000, 1, 0,    999);
    check("last byte only",        "bytes=999-",      1000, 1, 999,  999);
    check("end exactly at EOF",    "bytes=0-999",     1000, 1, 0,    999);

    /* Reject: malformed prefix */
    check("missing bytes prefix",  "0-99",            1000, -1, 0, 0);
    check("wrong unit",            "lines=0-99",      1000, -1, 0, 0);
    check("no equals",             "bytes 0-99",      1000, -1, 0, 0);

    /* Reject: structural malformations */
    check("no dash",               "bytes=100",       1000, -1, 0, 0);
    check("two dashes",            "bytes=10-20-30",  1000, -1, 0, 0);
    check("multi-range",           "bytes=0-99,200-299", 1000, -1, 0, 0);
    check("trailing comma",        "bytes=0-99,",     1000, -1, 0, 0);

    /* Reject: suffix-byte form (deliberately unsupported) */
    check("suffix form",           "bytes=-100",      1000, -1, 0, 0);
    check("just dash",             "bytes=-",         1000, -1, 0, 0);

    /* Reject: out-of-range */
    check("start past EOF",        "bytes=1000-",     1000, -1, 0, 0);
    check("end past EOF",          "bytes=0-1000",    1000, -1, 0, 0);
    check("both past EOF",         "bytes=2000-3000", 1000, -1, 0, 0);

    /* Reject: start > end */
    check("reversed range",        "bytes=500-100",   1000, -1, 0, 0);

    /* Reject: non-numeric */
    check("alphabetic start",      "bytes=abc-100",   1000, -1, 0, 0);
    check("alphabetic end",        "bytes=10-xyz",    1000, -1, 0, 0);
    check("space in start",        "bytes= 10-20",    1000, -1, 0, 0);
    check("plus sign",             "bytes=+10-20",    1000, -1, 0, 0);
    check("negative start",        "bytes=-10-20",    1000, -1, 0, 0);

    /* Reject: empty / NULL */
    check("empty header",          "",                1000, -1, 0, 0);
    check("NULL header",           NULL,              1000, -1, 0, 0);

    /* Reject: zero-size file */
    check("zero file size",        "bytes=0-",        0, -1, 0, 0);
    check("negative file size",    "bytes=0-",       -1, -1, 0, 0);

    /* Edge: very large file (still within long range) */
    check("large file open",       "bytes=1000000-",  10000000, 1, 1000000, 9999999);
    check("large file closed",     "bytes=0-9999999", 10000000, 1, 0,       9999999);

    fprintf(stderr, "\nresults: %d/%d passed\n",
            g_total - g_failures, g_total);
    return g_failures == 0 ? 0 : 1;
}
