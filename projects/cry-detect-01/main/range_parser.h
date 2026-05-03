#pragma once

/* Pure HTTP Range header parser. No IDF dependencies — host-testable.
 *
 * Parses values like "bytes=N-" or "bytes=N-M". Suffix-byte form ("bytes=-N")
 * and multi-range ("bytes=A-B,C-D") are not supported.
 *
 * Returns:
 *    1  parsed valid single range; *out_start and *out_end are set,
 *       inclusive of both endpoints.
 *   -1  malformed, multi-range, suffix-form, or unsatisfiable
 *       (start >= file_size, end >= file_size, start > end).
 *
 * Caller checks for the absence of the header before calling this; this
 * function does not handle the "no Range header" case (it would return -1
 * on an empty string). */

int range_parse_value(const char *hdr_value,
                      long file_size,
                      long *out_start,
                      long *out_end);
