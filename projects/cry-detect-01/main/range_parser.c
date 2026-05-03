#include "range_parser.h"

#include <stdlib.h>
#include <string.h>

int range_parse_value(const char *hdr_value, long file_size,
                      long *out_start, long *out_end)
{
    if (!hdr_value || !out_start || !out_end || file_size < 0) return -1;
    if (strncmp(hdr_value, "bytes=", 6) != 0) return -1;

    const char *p = hdr_value + 6;
    if (strchr(p, ',')) return -1;  /* multi-range */

    /* Find the dash. Must be present, must be exactly one. */
    const char *dash = strchr(p, '-');
    if (!dash) return -1;
    if (strchr(dash + 1, '-')) return -1;  /* extra dash */

    /* Suffix form "bytes=-N" — not supported. */
    if (dash == p) return -1;

    /* Parse start. Must be a non-negative integer. */
    char start_buf[24];
    size_t start_len = (size_t)(dash - p);
    if (start_len == 0 || start_len >= sizeof(start_buf)) return -1;
    memcpy(start_buf, p, start_len);
    start_buf[start_len] = '\0';
    /* Reject anything that's not pure digits (no whitespace, no sign). */
    for (size_t i = 0; i < start_len; ++i) {
        if (start_buf[i] < '0' || start_buf[i] > '9') return -1;
    }
    long start = atol(start_buf);

    /* End is optional. */
    long end;
    if (*(dash + 1) == '\0') {
        end = file_size - 1;
    } else {
        const char *end_str = dash + 1;
        for (const char *q = end_str; *q; ++q) {
            if (*q < '0' || *q > '9') return -1;
        }
        end = atol(end_str);
    }

    if (file_size == 0) return -1;
    if (start >= file_size || end >= file_size) return -1;
    if (start > end) return -1;

    *out_start = start;
    *out_end   = end;
    return 1;
}
