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
  unsigned int destinations_size;
  Destination destinations[CONFIG_ROUTE_MAX_DEPARTURES];
} PredictionsData;

typedef struct Stop {
  const char* id;
  unsigned int routes_size;
  PredictionsData predictions_data[CONFIG_STOP_MAX_ROUTES];
} Stop;
#endif
