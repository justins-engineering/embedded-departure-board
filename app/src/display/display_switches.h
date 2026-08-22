/** @file display_switches.h
 *  @brief LED display enable switch defines & functions
 */

#ifndef DISPLAY_SWITCHES_H
#define DISPLAY_SWITCHES_H

/** Display boxes this firmware drives.
 *
 * The sign's hardware shape rather than a per-sign setting: the board
 * overlay defines one display_switch_N node per box and this names the
 * same count, which display_switches.c asserts at build time. Every
 * switch is claimed and held off at boot regardless of how many panels a
 * particular sign has, because a switch with nothing connected to it
 * enables nothing. What lights a box is a shadow mapping entry naming its
 * position, so a sign with fewer panels simply names fewer and the rest
 * are never turned on.
 */
#define DISPLAY_BOX_CAPACITY 6

int init_display_switches(void);
int display_on(const int display_number);
int display_off(const int display_number);

#endif
