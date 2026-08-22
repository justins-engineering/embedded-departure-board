/** @file runtime_config.h
 *  @brief Runtime-tunable operational knobs, shadow-overridable.
 *
 * Same pattern as stop_id.h, generalized: each value boots as its Kconfig
 * default and can be overwritten at runtime from the pigeon shadow's
 * target_config (net/pigeon_client.c), so a fleet operator can retune a
 * deployed sign from the dashboard without a reflash. Values persist
 * across the poll loop but deliberately not across reboot: the shadow
 * re-syncs on boot, which IS the persistence mechanism.
 *
 * Build-time-only knobs stay in Kconfig on purpose: buffer/stack sizes
 * (STOP_JSON_BUF_SIZE, STOP_MAX_ROUTES, ROUTE_MAX_DEPARTURES), hardware
 * shape (LIGHT_SENSOR), the watchdog window
 * (MAX_TIME_INACTIVE_BEFORE_RESET_MS -- wdt_install_timeout() happens once
 * before wdt_setup(), it cannot be re-armed at runtime), and the Swiftly
 * hostname/path/prediction-count trio (the response-buffer and parser
 * sizing above is budgeted against them at build time).
 *
 * The display-box count used to be listed here as hardware shape. It is
 * not a Kconfig option at all now: the board overlay wires a fixed set of
 * display switches and DISPLAY_BOX_CAPACITY (display/display_switches.h)
 * names how many, while how many a given sign USES follows from the
 * shadow's display mapping naming that many positions.
 */
#ifndef RUNTIME_CONFIG_H
#define RUNTIME_CONFIG_H

/** Single-word reads/writes (one writer: the pigeon client thread) -- no
 * locking needed on this core, see runtime_config.c. */
int runtime_config_http_retry_count(void);
void runtime_config_http_retry_count_set(int value);

#endif  // RUNTIME_CONFIG_H
