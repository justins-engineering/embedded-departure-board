/** @file fota.h
 *  @brief Macros and function defines for the HTTP client.
 */

#ifndef FOTA_H
#define FOTA_H

// #include <zephyr/stats/stats.h>

// /* Define an example stats group; approximates seconds since boot. */
// STATS_SECT_START(smp_svr_stats)
// STATS_SECT_ENTRY(ticks)
// STATS_SECT_END;

// /* Assign a name to the `ticks` stat. */
// STATS_NAME_START(smp_svr_stats)
// STATS_NAME(smp_svr_stats, ticks)
// STATS_NAME_END(smp_svr_stats);

// /* Define an instance of the stats group. */
// extern STATS_SECT_DECL(smp_svr_stats) smp_svr_stats;
#include <zephyr/types.h>

int write_buffer_to_flash(char *data, size_t len, _Bool flush);

void image_validation(uint8_t area_id);

void image_info(uint8_t area_id);

void download_update(void);

#endif
