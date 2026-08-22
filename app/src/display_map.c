#include "display_map.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "update_stop.h"

LOG_MODULE_REGISTER(display_map);

/* Same locking contract as stop_id.c: written by the pigeon client thread
 * (display_map_set), read by main's update_stop path (display_map_get).
 * Held only for bounded copies, never across I/O. */
static K_MUTEX_DEFINE(display_map_lock);

/* Seeded from the compile-time DISPLAY_BOXES layout at first use -- the
 * Kconfig-default fallback when the shadow never supplies a "displays"
 * key (same convention as stop_id.c/runtime_config.c). */
static struct display_map_entry map[DISPLAY_BOX_CAPACITY];
static size_t map_count;

/* Distinct from synced below: seeded only records that the compiled
 * fallback has been loaded, which happens lazily on the first read. */
static bool seeded;

/* False until display_map_set() accepts a mapping. Guarded by the same
 * lock as the table, so an observer that sees it true is also guaranteed
 * to read the accepted mapping rather than the seed. */
static bool synced;

static void seed_from_display_boxes(void) {
  static const DisplayBox defaults[] = DISPLAY_BOXES;

  for (size_t i = 0; i < ARRAY_SIZE(defaults) && i < ARRAY_SIZE(map); i++) {
    strncpy(map[i].route, defaults[i].id, sizeof(map[i].route) - 1);
    map[i].route[sizeof(map[i].route) - 1] = '\0';
    map[i].direction = defaults[i].direction_code;
    map[i].position = (uint8_t)defaults[i].position;
  }
  map_count = ARRAY_SIZE(defaults) < ARRAY_SIZE(map) ? ARRAY_SIZE(defaults) : ARRAY_SIZE(map);
  seeded = true;
}

void display_map_get(struct display_map_entry* entries, size_t* count) {
  k_mutex_lock(&display_map_lock, K_FOREVER);

  if (!seeded) {
    seed_from_display_boxes();
  }

  memcpy(entries, map, map_count * sizeof(map[0]));
  *count = map_count;
  k_mutex_unlock(&display_map_lock);
}

void display_map_set(const struct display_map_entry* entries, size_t count) {
  if (count > ARRAY_SIZE(map)) {
    count = ARRAY_SIZE(map);
  }

  k_mutex_lock(&display_map_lock, K_FOREVER);
  memcpy(map, entries, count * sizeof(map[0]));
  map_count = count;
  seeded = true;
  synced = true;
  k_mutex_unlock(&display_map_lock);

  LOG_INF("Display map updated (%u entries)", (unsigned)count);
}

bool display_map_synced(void) {
  k_mutex_lock(&display_map_lock, K_FOREVER);
  bool s = synced;
  k_mutex_unlock(&display_map_lock);
  return s;
}
