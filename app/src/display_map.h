/** @file display_map.h
 *  @brief Runtime-mutable route -> display-box mapping.
 *
 * The LAST per-stop compile-time knob: DISPLAY_BOXES (update_stop.h) bakes
 * in which route/direction lands on which physical display box, which is
 * why per-stop branches (stop_73, stop_1670, ...) existed at all. This
 * module holds the same mapping in a small mutable table, seeded from
 * DISPLAY_BOXES as the fallback default, so net/pigeon_client.c can
 * repoint it at runtime from the shadow's "displays" target_config key --
 * with this, one firmware image serves any stop and the per-stop branch
 * model is fully obsolete (stop_id.h covers WHICH stop, this covers HOW
 * its routes are laid out).
 *
 * Per-box hardware parameters (color, brightness -- the latter a per-box
 * POWER LIMIT, see update_stop.h) deliberately stay compile-time: the
 * shadow remaps routes to boxes, it can never raise a box's drive
 * current.
 */
#ifndef DISPLAY_MAP_H
#define DISPLAY_MAP_H

#include <stddef.h>
#include <stdint.h>

/** Route id storage; matches DisplayBox.id's 4-chars-plus-NUL. */
#define DISPLAY_MAP_ROUTE_LEN 5

struct display_map_entry {
  char route[DISPLAY_MAP_ROUTE_LEN];
  char direction;   /* Swiftly directionId code, e.g. '0'/'1' */
  uint8_t position; /* physical display box, 0..CONFIG_NUMBER_OF_DISPLAY_BOXES-1 */
};

/** @brief Copy the current mapping into entries (sized
 *  CONFIG_NUMBER_OF_DISPLAY_BOXES) and return the entry count via count.
 *  Safe from any thread (same mutex pattern as stop_id.h). */
void display_map_get(struct display_map_entry* entries, size_t* count);

/** @brief Replace the WHOLE mapping (not a per-entry merge -- the shadow's
 *  "displays" array is the complete layout for the stop). count is clamped
 *  to CONFIG_NUMBER_OF_DISPLAY_BOXES; entries with out-of-range positions
 *  were rejected by the caller's validation (net/pigeon_client.c). Safe
 *  from any thread. */
void display_map_set(const struct display_map_entry* entries, size_t count);

#endif  // DISPLAY_MAP_H
