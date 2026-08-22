#include "stop_id.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(stop_id);

/* Written by the pigeon client thread (stop_id_set), read by main's
 * Swiftly request path (stop_id_get) -- the lock keeps a reader from
 * seeing a half-written ID mid-update. Held only for a bounded local
 * copy, never across I/O. */
static K_MUTEX_DEFINE(stop_id_lock);

char current_stop_id[STOP_ID_MAX_LEN] = CONFIG_STOP_ID;

/* False until stop_id_set() accepts its first value. Guarded by the same
 * lock as the buffer so an observer that sees it true is also guaranteed
 * to read the accepted value, not the compiled-in seed. */
static bool synced;

void stop_id_set(const char* id) {
  if (!id || !*id) {
    LOG_ERR("Refusing to set an empty stop ID");
    return;
  }

  k_mutex_lock(&stop_id_lock, K_FOREVER);

  bool changed = strncmp(current_stop_id, id, sizeof(current_stop_id)) != 0;

  if (changed) {
    snprintk(current_stop_id, sizeof(current_stop_id), "%s", id);
  }

  /* An unchanged value still counts as a sync: a shadow confirming the
   * compiled-in seed is an authoritative answer to "which stop", and the
   * boot gate reading this flag must open on it. */
  synced = true;
  k_mutex_unlock(&stop_id_lock);

  if (changed) {
    LOG_INF("Stop ID updated to: %s", id);
  }
}

bool stop_id_synced(void) {
  k_mutex_lock(&stop_id_lock, K_FOREVER);
  bool s = synced;
  k_mutex_unlock(&stop_id_lock);
  return s;
}

void stop_id_get(char* buf, size_t buf_len) {
  k_mutex_lock(&stop_id_lock, K_FOREVER);
  snprintk(buf, buf_len, "%s", current_stop_id);
  k_mutex_unlock(&stop_id_lock);
}
