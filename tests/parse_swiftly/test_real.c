/* Runs the real captured Swiftly payload through the real parser and prints
 * what update_stop()/update_routes() would actually see. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json/parse_swiftly.h"
#include "stop.h"

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <payload.json>\n", argv[0]);
    return 2;
  }

  FILE *f = fopen(argv[1], "rb");
  if (!f) {
    perror("open");
    return 2;
  }
  static char buf[8192];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = '\0';
  fclose(f);
  printf("payload bytes: %zu\n", n);

  /* Seed with a populated fetch so any stale carry-over is visible. */
  Stop stop = {.id = "1670"};
  stop.routes_size = 99;
  for (int i = 0; i < CONFIG_STOP_MAX_ROUTES; i++) {
    stop.predictions_data[i].destinations_size = 99;
    for (int j = 0; j < CONFIG_ROUTE_MAX_DEPARTURES; j++) {
      stop.predictions_data[i].destinations[j].min = 4242;
    }
  }

  int ret = parse_swiftly_json(buf, &stop);
  printf("parse_swiftly_json ret : %d%s\n", ret,
         ret == PARSE_SWIFTLY_NO_DEPARTURES ? "  (PARSE_SWIFTLY_NO_DEPARTURES)" : "");
  printf("stop.routes_size       : %u\n", stop.routes_size);

  int drawn = 0;
  for (unsigned int r = 0; r < stop.routes_size && r < CONFIG_STOP_MAX_ROUTES; r++) {
    PredictionsData *pd = &stop.predictions_data[r];
    printf("  route %-4s destinations_size=%u  mins:", pd->route_id, pd->destinations_size);
    for (unsigned int d = 0; d < pd->destinations_size && d < CONFIG_ROUTE_MAX_DEPARTURES; d++) {
      int m = pd->destinations[d].min;
      printf(" %d", m);
      /* update_routes() skips min == -1 and draws anything else. */
      if (m != -1) {
        drawn++;
      }
    }
    printf("\n");
  }
  printf("\nupdate_routes() would draw %d value(s); 0 means every display stays off\n", drawn);
  return 0;
}
