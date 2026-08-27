#ifndef UPDATE_STOP_H
#define UPDATE_STOP_H

#include <zephyr/kernel.h>

/* A sign's route-to-box mapping describes one physical sign, so it lives in an
 * untracked header rather than here. */
#if defined(__has_include)
#if __has_include("display_boxes.local.h")
#include "display_boxes.local.h"
#endif
#endif

#ifndef DISPLAY_BOXES
#error "No display box mapping. Copy app/src/display_boxes.example.h to app/src/display_boxes.local.h and set it for this sign."
#endif

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
