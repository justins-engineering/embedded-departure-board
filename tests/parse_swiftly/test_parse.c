/* Host harness for the Swiftly parse-and-classify path.
 * Compiles the real app/src/json sources against stub Zephyr headers so the
 * empty-departures behaviour can be asserted without touching the board. */
#include <stdio.h>
#include <string.h>

#include "json/parse_swiftly.h"
#include "stop.h"

static int failures;

static void check(const char *what, long got, long want) {
  if (got == want) {
    printf("  PASS  %-46s got %ld\n", what, got);
  } else {
    printf("  FAIL  %-46s got %ld, want %ld\n", what, got, want);
    failures++;
  }
}

/* Mirrors update_stop()'s classification of the parser's return value,
 * including the static Stop that persists across calls. */
static int classify(const char *json, Stop *stop, unsigned int *streak) {
  int ret = parse_swiftly_json(json, stop);

#ifdef FIXED
  if (ret == PARSE_SWIFTLY_NO_DEPARTURES) {
    *streak = 0;
    return 2;
  }
#endif
  if (ret) {
    (*streak)++;
    return 1;
  }
  *streak = 0;
  return 0;
}

/* A populated response: one route, one destination five minutes out. */
static const char POPULATED[] =
    "{\"success\":true,\"route\":\"G1\",\"data\":{\"agencyKey\":\"pvta\",\"predictionsData\":"
    "[{\"routeShortName\":\"G1\",\"destinations\":[{\"directionId\":\"0\",\"predictions\":"
    "[{\"min\":5,\"sec\":300}]}]}]}}";

/* Well-formed, but the predictionsData array is empty -- what the API
 * returns when the stop simply has nothing upcoming. */
static const char EMPTY_PREDICTIONS[] =
    "{\"success\":true,\"route\":\"G1\",\"data\":{\"agencyKey\":\"pvta\","
    "\"predictionsData\":[]}}";

/* A degenerate body: fewer than two tokens, the case parse_swiftly_json
 * reports as "No scheduled departures". */
static const char DEGENERATE[] = "{}";

int main(void) {
  Stop stop = {.id = "1670"};
  unsigned int streak = 0;

  printf("populated response\n");
  int r = classify(POPULATED, &stop, &streak);
  check("update_stop() return", r, 0);
  check("failure streak", streak, 0);
  check("routes_size", stop.routes_size, 1);

  printf("\ndegenerate body (parser's no-departures branch)\n");
  streak = 0;
  r = classify(DEGENERATE, &stop, &streak);
#ifdef FIXED
  check("update_stop() return", r, 2);
  check("failure streak", streak, 0);
#else
  check("update_stop() return", r, 1);
  check("failure streak", streak, 1);
#endif

  printf("\nempty predictionsData, after a populated fetch\n");
  streak = 0;
  Stop stop2 = {.id = "1670"};
  (void)classify(POPULATED, &stop2, &streak);
  unsigned int before = stop2.routes_size;
  r = classify(EMPTY_PREDICTIONS, &stop2, &streak);
  check("routes_size before", before, 1);
  check("update_stop() return", r, 0);
  check("failure streak", streak, 0);
  check("routes_size after (0 == sign blanks)", stop2.routes_size, 0);

  printf("\n%s (%d failing assertion(s))\n", failures ? "FAILURES" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
