#ifndef UPDATE_STOP_H
#define UPDATE_STOP_H

#include <zephyr/kernel.h>

/* A sign's route-to-box mapping describes one physical sign, so a build takes
 * it from an untracked header when the deployment supplies one. */
#if defined(__has_include)
#if __has_include("display_boxes.local.h")
#include "display_boxes.local.h"
#endif
#endif

#ifndef DISPLAY_BOXES
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
#endif  // DISPLAY_BOXES

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
int update_stop(void);

extern struct k_timer update_stop_timer;
extern struct k_sem update_stop_sem;

#endif
