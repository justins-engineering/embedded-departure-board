#ifndef UPDATE_STOP_H
#define UPDATE_STOP_H

#include <zephyr/kernel.h>

/** Specify the route id, position, direction, color, and brightness for each display box */
// clang-format off
#define DISPLAY_BOXES {                                                                          \
  { .id = "R29",  .position = 0, .direction_code = '1', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "38",   .position = 1, .direction_code = '1', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "B43",  .position = 2, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "B43",  .position = 3, .direction_code = '1', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "943",  .position = 4, .direction_code = '1', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "B79",  .position = 5, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }  \
}
// clang-format on

/** @param brightness The max brightness allowed with all LEDS (888) on  without
 * going above the 126mA per display limit */
typedef const struct DisplayBox {
  const char id[5];
  const char direction_code;
  const int position;
  const uint32_t color;
  const uint8_t brightness;
} DisplayBox;

void update_stop_timeout_handler(struct k_timer* timer_id);

/** @brief update_stop() fetched successfully but the stop has no departures.
 *
 * A separate result from 0 because callers light the display differently for
 * an empty stop, and separate from the failure return because the fetch
 * worked: it must not feed the reset streak or the fetch-health telemetry.
 * Overnight, when empty is the correct answer for hours, treating it as a
 * failure would be indistinguishable from the network being down.
 */
#define UPDATE_STOP_NO_DEPARTURES 2

int update_stop(void);

extern struct k_timer update_stop_timer;
extern struct k_sem update_stop_sem;

/** @brief Consecutive update_stop() failures since the last success.
 * Read by net/pigeon_client.c for the swiftly_consecutive_failures
 * telemetry key -- 0 means the most recent update_stop() call succeeded.
 */
unsigned int swiftly_consecutive_failures(void);

/** @brief Seconds of uptime since the last successful update_stop() call.
 * Read by net/pigeon_client.c for the swiftly_last_success_age_s telemetry
 * key. Returns -1 if update_stop() has never succeeded yet this boot.
 */
int swiftly_last_success_age_s(void);

#endif
