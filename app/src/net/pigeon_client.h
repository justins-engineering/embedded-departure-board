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

extern struct k_timer pigeon_poll_timer;
extern struct k_sem pigeon_poll_sem;

/** @brief Initialize the pigeon library and apply whatever shadow config
 *  already exists (falling back to this build's baked-in defaults --
 *  CONFIG_STOP_ID via stop_id.h -- if the shadow has none yet).
 *
 * Call once, after LTE is up (mirrors https_init's pigeon_init() ordering).
 * Starts pigeon_poll_timer on CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS;
 * the shadow's own "telemetry_interval" (if present) restarts it with a
 * different period the first time a poll applies one.
 */
void pigeon_client_init(void);

/** @brief One shadow sync + telemetry report cycle.
 *
 * Call from the main loop whenever pigeon_poll_sem is available (same
 * pattern as update_stop_sem/update_stop()). Never blocks the caller for
 * more than a handful of short HTTPS request/response round trips.
 */
void pigeon_client_poll(void);

#endif  // PIGEON_CLIENT_H
