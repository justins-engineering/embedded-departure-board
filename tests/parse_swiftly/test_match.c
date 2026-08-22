/* Covers the display-mapping lookup once it accepts either spelling of a
 * route. Mirrors get_display_address()'s matching rule rather than linking
 * update_stop.c, which drags in the display drivers and the Zephyr kernel. */
#include <stdio.h>
#include <string.h>

#include "display/display_switches.h"
#include "json/parse_swiftly.h"
#include "stop.h"
#include "update_stop.h"

struct map_entry {
  const char *route;
  char direction;
  int position;
};

static int failures;

static void check(const char *what, int got, int want) {
  if (got == want) {
    printf("  PASS  %-52s got %d\n", what, got);
  } else {
    printf("  FAIL  %-52s got %d, want %d\n", what, got, want);
    failures++;
  }
}

/* The rule under test, kept in step with update_stop.c's version. */
static int lookup(
    const struct map_entry map[], size_t n, const char *route_id, const char *route_short_name,
    char direction
) {
  for (size_t i = 0; i < n; i++) {
    if (map[i].direction != direction) {
      continue;
    }
    if (!strncmp(route_id, map[i].route, 4) || !strncmp(route_short_name, map[i].route, 4)) {
      return map[i].position;
    }
  }
  return -1;
}

/* Mirrors box_params_for_position()'s scan, for the same reason lookup()
 * mirrors get_display_address(): linking update_stop.c would drag in the
 * display drivers and the kernel. The TABLE is the real one. */
static const DisplayBox *params_for_position(int position) {
  static const DisplayBox boxes[] = DISPLAY_BOXES;

  for (size_t i = 0; i < sizeof(boxes) / sizeof(boxes[0]); i++) {
    if (boxes[i].position == position) {
      return &boxes[i];
    }
  }
  return NULL;
}

static const char REAL[] =
    "{\"success\":true,\"data\":{\"agencyKey\":\"pvta\",\"predictionsData\":"
    "[{\"routeShortName\":\"G1\",\"routeId\":\"G101\",\"destinations\":"
    "[{\"directionId\":\"0\",\"predictions\":[{\"min\":4}]}]}]}}";

int main(void) {
  /* Both spellings of the same route reach the same display. */
  printf("either spelling resolves to the same display\n");
  const struct map_entry by_id[] = {{"G101", '0', 0}};
  const struct map_entry by_short[] = {{"G1", '0', 0}};
  check("mapping written as the id  -> position", lookup(by_id, 1, "G101", "G1", '0'), 0);
  check("mapping written as the name -> position", lookup(by_short, 1, "G101", "G1", '0'), 0);

  /* The ambiguity accepting both fields makes reachable: one route's short
   * name equal to a different route's id. Whichever entry comes first wins,
   * so a mapping meant for the other route is claimed by this one. This is
   * the reason the field was documented as an id; it is asserted here so the
   * behaviour is recorded rather than discovered. */
  printf("\ncollision: one route's short name equals another route's id\n");
  const struct map_entry collide[] = {{"X92", '0', 1}};
  /* Route A: id X92. Route B: short name X92, id B77. Both match entry 0. */
  check("route with id X92        claims the entry", lookup(collide, 1, "X92", "A1", '0'), 1);
  check("route with SHORT name X92 also claims it", lookup(collide, 1, "B77", "X92", '0'), 1);
  printf("  NOTE: both resolve to position 1 -- accepting either field makes this\n");
  printf("        reachable. Absent at the stop in use; check before adding routes.\n");

  /* Direction still gates the match. */
  printf("\ndirection still has to agree\n");
  check("right route, wrong direction -> no match", lookup(by_short, 1, "G101", "G1", '1'), -1);

  /* Every box the mapping validator will accept a position for has
   * parameters in the table, so a validated position always resolves to a
   * real display. This is what lets one image serve a sign with fewer
   * panels: the table stays full-length and the sign simply names fewer
   * positions. */
  printf("\nevery box position has hardware parameters\n");
  for (int p = 0; p < DISPLAY_BOX_CAPACITY; p++) {
    char what[52];
    snprintf(what, sizeof(what), "position %d resolves to a parameters entry", p);
    check(what, params_for_position(p) != NULL, 1);
  }
  check(
      "a position past the capacity resolves to nothing",
      params_for_position(DISPLAY_BOX_CAPACITY) == NULL, 1
  );

  /* Three routes on a six-box image, and deliberately not on the first
   * three boxes. This is the shape a sign with fewer panels produces, and
   * the case that breaks the moment the active set is taken to be a
   * prefix of the boxes rather than the positions the mapping names. */
  printf("\nsparse mapping, positions not a prefix\n");
  const struct map_entry sparse[] = {{"G1", '0', 0}, {"G2", '0', 2}, {"X92", '1', 5}};
  check("first entry keeps its position", lookup(sparse, 3, "G1", "G1", '0'), 0);
  check("second entry skips a box", lookup(sparse, 3, "G2", "G2", '0'), 2);
  check("third entry lands on the last box", lookup(sparse, 3, "X92", "X92", '1'), 5);
  check("a route the mapping omits matches nothing", lookup(sparse, 3, "B43", "B43", '0'), -1);
  printf("  NOTE: boxes 1, 3 and 4 are named by nothing, so update_routes writes\n");
  printf("        none of them and its per-pass sweep leaves them dark.\n");

  /* The parser really does populate both fields from a real-shaped payload. */
  printf("\nparser populates both fields\n");
  Stop stop = {.id = "1670"};
  int ret = parse_swiftly_json(REAL, &stop);
  check("parse returns success", ret, 0);
  check("routes_size", (int)stop.routes_size, 1);
  check("route_id is G101", strcmp(stop.predictions_data[0].route_id, "G101") == 0, 1);
  check("route_short_name is G1", strcmp(stop.predictions_data[0].route_short_name, "G1") == 0, 1);

  printf("\n%s (%d failing assertion(s))\n", failures ? "FAILURES" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
