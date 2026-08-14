#ifndef STOP_H
#define STOP_H

#include <stdint.h>

typedef struct Destination {
  char direction_id;
  int min;
  /* Swiftly's mid-2026 occupancy keys, parsed from the same prediction
   * object as "min" (parse_swiftly.c). -1 == not present in the payload
   * (both are optional, per-vehicle APC data): occupancy_percent is
   * 0-100, occupancy_count a passenger count. Model-only for now --
   * display/telemetry surfacing is an open product decision. */
  int16_t occupancy_percent;
  int16_t occupancy_count;
} Destination;

typedef struct PredictionsData {
  char route_id[5];
  /* The name printed on the bus, stored beside the internal id because a
   * display mapping may legitimately be written with either. The two are
   * usually the same string and occasionally are not -- this agency's G1
   * carries the id G101 -- and only the id was kept, so a mapping authored
   * with the short name matched nothing and left that display dark. */
  char route_short_name[5];
  unsigned int destinations_size;
  Destination destinations[CONFIG_ROUTE_MAX_DEPARTURES];
} PredictionsData;

typedef struct Stop {
  const char* id;
  unsigned int routes_size;
  PredictionsData predictions_data[CONFIG_STOP_MAX_ROUTES];
} Stop;
#endif
