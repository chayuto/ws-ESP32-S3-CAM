#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Lightweight "what was I doing" trail persisted across reboots in NVS.
 * Writes a single breadcrumb per call — always overwriting the same slot,
 * so the last-written value is what survives a panic. Complements the
 * coredump (which captures panics) by also covering silent reboots:
 * brown-out, WDT, assert-before-coredump-init, etc.
 *
 * Payload stored as JSON in NVS string key "bc/last":
 *   {"stage":"<name>","up":<uptime_s>,"epoch":<time_t>,"boot":<n>}
 *
 * Do NOT call breadcrumb_set from an ISR or the esp_event loop task —
 * NVS writes can block tens of ms (flash erase). Call from
 * housekeeping_task or similar worker context. */

/* Called once from app_main. Reads and logs the previous-boot breadcrumb
 * (if any) then advances the boot counter. */
void breadcrumb_init(void);

/* Set a new breadcrumb. Overwrites previous value. */
void breadcrumb_set(const char *stage);

/* Fill buf with a JSON object of the current (RAM) breadcrumb state plus
 * the previous-boot value read at init. Returns bytes written.
 * Format: {"now":{...}, "prev_boot":{...}|null, "boot_counter":N}
 * Safe from any task — reads cached in-memory copies. */
size_t breadcrumb_status_json(char *buf, size_t max);

/* Boot counter from NVS (monotonic across reboots). */
uint32_t breadcrumb_boot_counter(void);

#ifdef __cplusplus
}
#endif
