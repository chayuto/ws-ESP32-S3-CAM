#pragma once

#include "sdkconfig.h"

/* Phase B: write a one-shot session marker file to /sdcard once SD is
 * mounted AND NTP has synced. Format:
 *   /sdcard/.session-started-<ISO8601>.json
 * Body has firmware version, boot_count, generation_id, mounted_at.
 *
 * Idempotent: stores the boot_count it last wrote a marker for in NVS;
 * subsequent calls in the same boot are no-ops. Calling once per
 * housekeeping tick is the expected pattern.
 *
 * Compiled out when CONFIG_CRY_SYNC_SESSION_MARKER is off — declarations
 * still resolve so call sites don't need #ifdef. */

void session_marker_maybe_write(void);
