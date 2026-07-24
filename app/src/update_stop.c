#include "update_stop.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display/display_switches.h"
#include "display/led_display.h"
#include "json/parse_swiftly.h"
#include "net/custom_http_client.h"
#include "stop.h"
#include "stop_id.h"

LOG_MODULE_REGISTER(update_stop);

K_TIMER_DEFINE(update_stop_timer, update_stop_timeout_handler, NULL);

K_SEM_DEFINE(update_stop_sem, 1, 1);

static DisplayBox* get_display_address(
    const DisplayBox display_boxes[], const char* route_id, const char direction_code
) {
  for (size_t box = 0; box < CONFIG_NUMBER_OF_DISPLAY_BOXES; box++) {
    if (!strncmp(route_id, display_boxes[box].id, 4) &&
        (display_boxes[box].direction_code == direction_code)) {
      return &display_boxes[box];
    }
  }
  return NULL;
}

static int update_routes(Stop stop, DisplayBox display_boxes[]) {
  unsigned int times[6] = {0};

  for (size_t box = 0; box < CONFIG_NUMBER_OF_DISPLAY_BOXES; box++) {
    (void)display_off(box);
  }

  for (size_t route_num = 0; route_num < stop.routes_size; route_num++) {
    struct PredictionsData prediction_data = stop.predictions_data[route_num];
    LOG_INF(
        "Route ID: %s; Destinations size: %d", prediction_data.route_id,
        prediction_data.destinations_size
    );
    for (size_t departure_num = 0; departure_num < prediction_data.destinations_size;
         departure_num++) {
      struct Destination destination = prediction_data.destinations[departure_num];
      if (destination.min == -1) {
        continue;
      }

      DisplayBox* display =
          get_display_address(display_boxes, prediction_data.route_id, destination.direction_id);
      if (display != NULL) {
        LOG_INF(
            "  Display address: %d, Direction Code: %c, Minutes to departure: %d",
            display->position, destination.direction_id, destination.min
        );
        if ((times[display->position] == 0) || (destination.min < times[display->position])) {
          times[display->position] = destination.min;
          if (write_num_to_display(display, display->brightness, destination.min)) {
            return 1;
          }
        }
#ifdef CONFIG_DEBUG
        else {
          LOG_INF(
              "Display %u has lower time displayed;\nCurrent: %u\nAttempted: "
              "%u",
              display->position, times[display->position], destination.min
          );
        }
#endif
      } else {
        LOG_INF(
            "Display address for Route: %s, Direction Code: %c not found. Minutes to "
            "departure: %d",
            prediction_data.route_id, destination.direction_id, destination.min
        );
      }
    }
  }
  return 0;
}

/* Swiftly fetch health, read by net/pigeon_client.c for telemetry -- see
 * update_stop.h's docs. */
static unsigned int failure_streak;
static int64_t last_success_uptime_ms = -1;

unsigned int swiftly_consecutive_failures(void) { return failure_streak; }

int swiftly_last_success_age_s(void) {
  if (last_success_uptime_ms < 0) {
    return -1;
  }
  return (int)((k_uptime_get() - last_success_uptime_ms) / 1000);
}

int update_stop(void) {
  int ret;
  /* .id points at the mutable current_stop_id buffer (stop_id.h) rather
   * than a CONFIG_STOP_ID literal, so a stop_id shadow update (see
   * net/pigeon_client.c) takes effect on the next call without a
   * reflash -- see stop_id.h's own docs for why this exists. */
  static Stop stop = {.id = current_stop_id};
  static const DisplayBox display_boxes[] = DISPLAY_BOXES;

  static char headers_buf[2048];

  /** HTTP response body buffer with size defined by the
   * CONFIG_STOP_JSON_BUF_SIZE
   */
  static char json_buf[CONFIG_STOP_JSON_BUF_SIZE];

  ret = http_request_stop_json(
      &json_buf[0], CONFIG_STOP_JSON_BUF_SIZE, headers_buf, sizeof(headers_buf)
  );
  if (ret) {
    LOG_ERR("HTTP GET request for JSON failed; cleaning up. ERR: %d", ret);
    failure_streak++;
    return 1;
  }

  ret = parse_swiftly_json(&json_buf[0], &stop);
  if (ret) {
    failure_streak++;
    return 1;
  }

  ret = update_routes(stop, display_boxes);
  if (ret) {
    failure_streak++;
    return 1;
  }

  failure_streak = 0;
  last_success_uptime_ms = k_uptime_get();
  return 0;
}

void update_stop_timeout_handler(struct k_timer* timer_id) { (void)k_sem_give(&update_stop_sem); }
