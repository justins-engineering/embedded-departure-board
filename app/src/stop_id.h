/** @file stop_id.h
 *  @brief Runtime-mutable bus stop ID.
 *
 * CONFIG_STOP_ID (Kconfig) used to be baked in at compile time and consumed
 * directly by update_stop.c/custom_http_client.c -- the whole reason this
 * project historically shipped a separate branch per stop (see git history/
 * README). This module holds the same value in a small mutable buffer
 * instead, seeded from CONFIG_STOP_ID as the fallback default, so
 * net/pigeon_client.c can repoint it at runtime from the shadow's
 * "stop_id" target_config key -- one firmware image, dashboard re-stops a
 * sign instead of a rebuild+reflash.
 */
#ifndef STOP_ID_H
#define STOP_ID_H

/** Long enough for any real Swiftly stop ID with room to spare (the
 * existing CONFIG_STOP_ID values in use are 2-3 digits; Stop.route_id
 * elsewhere in this codebase caps at 4 chars for the same kind of ID). */
#define STOP_ID_MAX_LEN 16

/** Current stop ID, initialized from CONFIG_STOP_ID. Read directly (e.g.
 * Stop.id in update_stop.c) -- always NUL-terminated, never written to
 * except via stop_id_set(). */
extern char current_stop_id[STOP_ID_MAX_LEN];

/** @brief Overwrite the current stop ID.
 *  @param id New stop ID, truncated (not rejected) if it doesn't fit
 *  STOP_ID_MAX_LEN - 1 bytes -- a truncated-but-wrong stop ID is a visible,
 *  debuggable failure (wrong/no departures shown); silently refusing the
 *  update would look identical to the shadow poll never having run at all.
 */
void stop_id_set(const char *id);

#endif  // STOP_ID_H
