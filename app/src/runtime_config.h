/** @file runtime_config.h
 *  @brief Runtime-tunable operational knobs, shadow-overridable.
 *
 * Same pattern as stop_id.h, generalized: each value boots as its Kconfig
 * default and can be overwritten at runtime from the pigeon shadow's
 * target_config (net/pigeon_client.c), so a fleet operator can retune a
 * deployed sign -- NTP servers, timeouts, retry budgets -- from the
 * dashboard without a reflash. Values persist across the poll loop but
 * deliberately not across reboot: the shadow re-syncs on boot, which IS
 * the persistence mechanism.
 *
 * Build-time-only knobs stay in Kconfig on purpose: buffer/stack sizes
 * (STOP_JSON_BUF_SIZE, STOP_MAX_ROUTES, ROUTE_MAX_DEPARTURES), hardware
 * shape (NUMBER_OF_DISPLAY_BOXES, LIGHT_SENSOR), the watchdog window
 * (MAX_TIME_INACTIVE_BEFORE_RESET_MS -- wdt_install_timeout() happens once
 * before wdt_setup(), it cannot be re-armed at runtime), and the Swiftly
 * hostname/path/prediction-count trio (the response-buffer and parser
 * sizing above is budgeted against them at build time).
 */
#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

#include <stddef.h>

/** Fits every real NTP pool hostname in use with headroom; a shadow value
 * that doesn't fit is truncated by the setter's snprintk, which the next
 * NTP query surfaces as a visible resolution failure (same debuggability
 * rationale as stop_id.h's truncate-don't-reject). */
#define RUNTIME_NTP_SERVER_MAX_LEN 48

/** @brief Copy the current primary/fallback NTP servers (NUL-terminated,
 *  truncating). Safe from any thread. */
void runtime_config_ntp_servers_get(
    char* primary, size_t primary_len, char* fallback, size_t fallback_len
);

/** @brief Overwrite the NTP servers. NULL/empty leaves a value unchanged. */
void runtime_config_ntp_servers_set(const char* primary, const char* fallback);

/** Single-word reads/writes (one writer: the pigeon client thread) -- no
 * locking needed on this core, see runtime_config.c. */
int runtime_config_ntp_timeout_ms(void);
void runtime_config_ntp_timeout_ms_set(int value);
int runtime_config_ntp_retry_count(void);
void runtime_config_ntp_retry_count_set(int value);
int runtime_config_http_retry_count(void);
void runtime_config_http_retry_count_set(int value);

#endif  // RUNTIME_CONFIG_H
