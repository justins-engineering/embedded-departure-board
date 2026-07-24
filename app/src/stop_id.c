#include "stop_id.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(stop_id);

char current_stop_id[STOP_ID_MAX_LEN] = CONFIG_STOP_ID;

void stop_id_set(const char* id) {
  if (!id || !*id) {
    LOG_ERR("Refusing to set an empty stop ID");
    return;
  }

  if (strncmp(current_stop_id, id, sizeof(current_stop_id)) == 0) {
    return;
  }

  snprintk(current_stop_id, sizeof(current_stop_id), "%s", id);

  LOG_INF("Stop ID updated to: %s", current_stop_id);
}
