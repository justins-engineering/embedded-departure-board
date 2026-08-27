#ifndef DISPLAY_BOXES_EXAMPLE_H
#define DISPLAY_BOXES_EXAMPLE_H

/* Copy this file to app/src/display_boxes.local.h and replace the entries with
 * the route-to-box mapping of the sign being built. The mapping is untracked
 * for the same reason the stop id is: it describes one physical sign.
 *
 * One entry per display box, and exactly CONFIG_NUMBER_OF_DISPLAY_BOXES of
 * them. A route id is at most four characters. The direction code is the single
 * character the predictions API uses for a direction of travel. Brightness is a
 * per-box power cap chosen so that a box with every LED lit stays inside its
 * current limit, which is why it lives here and not in a shadow the platform
 * can raise.
 */

/** Specify the route id, position, direction, color, and brightness for each display box */
// clang-format off
#define DISPLAY_BOXES {                                                                          \
  { .id = "RT1",  .position = 0, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "RT2",  .position = 1, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "RT3",  .position = 2, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "RT4",  .position = 3, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "RT5",  .position = 4, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }, \
  { .id = "RT6",  .position = 5, .direction_code = '0', .color = 0x00FF00, .brightness = 0x45 }  \
}
// clang-format on

#endif  // DISPLAY_BOXES_EXAMPLE_H
