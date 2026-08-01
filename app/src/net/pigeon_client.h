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

#endif  // PIGEON_CLIENT_H
