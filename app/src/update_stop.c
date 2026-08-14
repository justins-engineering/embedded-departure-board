#include "update_stop.h"

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "display/display_switches.h"
#include "display/led_display.h"
#include "display_map.h"
#include "json/parse_swiftly.h"
#include "net/custom_http_client.h"
#include "stop.h"
#include "stop_id.h"

LOG_MODULE_REGISTER(update_stop);

K_TIMER_DEFINE(update_stop_timer, update_stop_timeout_handler, NULL);

K_SEM_DEFINE(update_stop_sem, 1, 1);

/* Hardware parameters (brightness power cap, color) live in the static
 * DISPLAY_BOXES table keyed by physical position; the route->position
 * mapping itself is runtime (display_map.h, shadow-tunable). */
static DisplayBox* box_params_for_position(const DisplayBox display_boxes[], uint8_t position) {
  for (size_t box = 0; box < CONFIG_NUMBER_OF_DISPLAY_BOXES; box++) {
    if (display_boxes[box].position == position) {
      return &display_boxes[box];
    }
  }
  return NULL;
}

/* A mapping entry matches on either spelling of the route. The id is what
 * the feed calls it internally; the short name is what is printed on the
 * bus and on the timetable, and is therefore what someone authoring a
 * mapping will reach for. They agree for most routes and not for all --
 * this agency's G1 is served under the id G101 -- so accepting only the id
 * meant a plausibly-written mapping matched nothing at all, silently.
 *
 * Accepting both makes an ambiguity reachable that the id alone did not
 * have: one route's short name could equal a different route's id, and the
 * first entry in the map would win. That is not hypothetical in general
 * transit data, only absent from this stop, so anyone extending this should
 * check for it rather than assume. Direction still has to agree either way,
 * which narrows it further. */
static DisplayBox* get_display_address(
    const DisplayBox display_boxes[], const struct display_map_entry map[], size_t map_count,
    const char* route_id, const char* route_short_name, const char direction_code
) {
  for (size_t i = 0; i < map_count; i++) {
    if (map[i].direction != direction_code) {
      continue;
    }
    if (!strncmp(route_id, map[i].route, 4) || !strncmp(route_short_name, map[i].route, 4)) {
      return box_params_for_position(display_boxes, map[i].position);
    }
  }
  return NULL;
}

static int update_routes(
    Stop stop, DisplayBox display_boxes[], const struct display_map_entry map[], size_t map_count
) {
  unsigned int times[6] = {0};

  /* Summarised once at the end of the pass rather than logged per
   * destination. A route that matches no display mapping does so on every
   * destination of every pass, so the per-destination form buried the fact
   * in thousands of identical lines a day -- which is why it was kept at a
   * level release builds drop, and why the mismatch that dark-started a
   * display went unseen through a production cutover. Only the first
   * offender is kept, so this costs a fixed few bytes on a stack that is
   * already the tight resource here; the count tells a reader whether
   * there are others to find after fixing that one. */
  unsigned int unmatched = 0;
  char unmatched_route[sizeof(stop.predictions_data[0].route_id)] = {0};
  char unmatched_direction = '\0';

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

      DisplayBox* display = get_display_address(
          display_boxes, map, map_count, prediction_data.route_id,
          prediction_data.route_short_name, destination.direction_id
      );
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
        if (unmatched == 0) {
          /* Whole fixed-size array, NUL included: the parser always
           * terminates route_id, and both are the same char[N], so this
           * copies the terminator rather than relying on a bound that
           * stops one byte short of it. */
          memcpy(unmatched_route, prediction_data.route_id, sizeof(unmatched_route));
          unmatched_direction = destination.direction_id;
        }
        unmatched++;
      }
    }
  }

  /* A warning rather than info because it is always a real fault: this is
   * only reached for a departure the API actually returned, so it means a
   * bus is due and no display will show it. It also cannot chatter on a
   * healthy sign -- a correct mapping never reaches here at all, and an
   * empty stop skips this branch entirely -- so it stays silent until
   * something is genuinely misconfigured, then keeps saying so until it is
   * fixed. The route id printed is the one the API returns, which is what
   * the shadow's mapping has to match. */
  if (unmatched > 0) {
    LOG_WRN(
        "%u departure(s) had no display mapping; first was route %s direction %c",
        unmatched, unmatched_route, unmatched_direction
    );
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

  /* 1024 (was 2048, RAM diet 2026-08-08): holds the OUTGOING request
   * (~300B, built in place by send_http_request) and then the response
   * header block (Swiftly via CloudFront: ~600-800B observed). Overflow
   * is now a bounded, logged error (parse_headers), not a buffer walk. */
  static char headers_buf[1024];

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
  if (ret == PARSE_SWIFTLY_NO_DEPARTURES) {
    /* A fetch that correctly reports nothing upcoming is a healthy fetch, so
     * it clears the streak and refreshes the success timestamp exactly as a
     * populated one does. Counting it as a failure would let a quiet stop
     * accumulate a reset streak and would age swiftly_last_success_age_s
     * without anything being wrong. */
    failure_streak = 0;
    last_success_uptime_ms = k_uptime_get();
    return UPDATE_STOP_NO_DEPARTURES;
  }
  if (ret) {
    failure_streak++;
    return 1;
  }

  /* Snapshotted per pass, same cross-thread contract as stop_id: the
   * pigeon client thread may swap the mapping between passes, never
   * mid-pass. */
  struct display_map_entry map[CONFIG_NUMBER_OF_DISPLAY_BOXES];
  size_t map_count = 0;

  display_map_get(map, &map_count);

  ret = update_routes(stop, display_boxes, map, map_count);
  if (ret) {
    failure_streak++;
    return 1;
  }

  failure_streak = 0;
  last_success_uptime_ms = k_uptime_get();
  return 0;
}

void update_stop_timeout_handler(struct k_timer* timer_id) { (void)k_sem_give(&update_stop_sem); }
