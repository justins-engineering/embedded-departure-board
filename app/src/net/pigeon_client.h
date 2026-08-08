/** @file pigeon_client.h
 *  @brief PidgeIoT device management integration for this sign.
 *
 * Wraps the `pigeon` library (see app/west.yml) to make this board a
 * managed pigeon: the dashboard can push a new stop_id via the shadow
 * instead of this app needing a per-stop branch/rebuild (see stop_id.h),
 * plus periodic telemetry (signal quality, Swiftly fetch health, uptime).
 */
#ifndef PIGEON_CLIENT_H
#define PIGEON_CLIENT_H

#include <zephyr/kernel.h>

/** @brief Initialize the pigeon library and start the pigeon client thread.
 *
 * Call once, after LTE is up. Never blocks: all pigeon network I/O
 * (shadow sync, telemetry, and their failure/retry handling) runs on a
 * dedicated low-priority thread, so a slow or unreachable PidgeIoT can
 * never stall main's display loop, delay boot, or starve the hardware
 * watchdog main feeds. On pigeon_init() failure the sign simply runs
 * unmanaged this boot (Kconfig-default stop_id, no telemetry).
 *
 * The poll cadence starts at CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS;
 * the shadow's own "telemetry_interval" (if present) re-paces it, and
 * consecutive failed cycles back off to at most 4x the interval.
 */
void pigeon_client_init(void);

/** @brief Whether a FOTA download is currently streaming.
 *
 * Read by main's loop to SOFTEN (not remove) its reset-on-fetch-failure
 * policy while an over-the-air download saturates the LTE link: a failed
 * Swiftly cycle during a download logs and skips instead of rebooting,
 * because the reboot kills the download and burns a persisted FOTA
 * attempt (observed live: all 3 attempts of the first OTA test died this
 * way). BOUNDED: self-expires ~40 min after download start even if the
 * flag is stuck, so a wedged download can never disable the sign's
 * self-recovery permanently. Always false when CONFIG_PIGEON_FOTA is
 * off. Safe from any thread. */
bool pigeon_client_fota_active(void);

/** @brief The update_stop (Swiftly fetch) interval currently in force, in
 * seconds -- CONFIG_UPDATE_STOP_FREQUENCY_SECONDS until a shadow supplies
 * update_stop_interval (clamped 5-45s, see pigeon_client.c).
 *
 * Read by custom_http_client.c's RAI end-of-data signaling: whether an
 * early RRC release helps or thrashes depends on how the fetch cadence
 * compares to the carrier's own inactivity timer. Plain aligned int read,
 * written on the pigeon thread only -- safe from any thread. */
int pigeon_client_update_stop_interval_s(void);

#endif  // PIGEON_CLIENT_H
