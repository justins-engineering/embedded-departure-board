/* The display-mapping half of the boot gate's seam, mirroring test_sync.c
 * for stop_id: the table starts as the compiled-in DISPLAY_BOXES layout
 * with display_map_synced() false, and only a mapping accepted at runtime
 * flips it true. Compiles the real app/src/display_map.c against stub
 * headers.
 *
 * The flag is monotonic, so case order matters: everything asserting
 * "still unsynced" has to run before the first accepted set. The lazy
 * seed-on-first-read is the case most worth pinning down, since a seed
 * that counted as a sync would hand the compiled layout the authority
 * that only an operator's mapping is supposed to have. */
#include <stdio.h>
#include <string.h>

#include "display_map.h"

static int failures;

static void check(const char *what, int got, int want) {
  if (got == want) {
    printf("  PASS  %-52s got %d\n", what, got);
  } else {
    printf("  FAIL  %-52s got %d, want %d\n", what, got, want);
    failures++;
  }
}

int main(void) {
  struct display_map_entry entries[DISPLAY_BOX_CAPACITY];
  size_t count = 0;

  printf("boot state\n");
  check("nothing applied yet is not synced", display_map_synced(), 0);

  display_map_get(entries, &count);
  check("seed fills every box from DISPLAY_BOXES", (int)count, DISPLAY_BOX_CAPACITY);
  check("reading the seed does not count as a sync", display_map_synced(), 0);

  printf("\na shorter mapping, as a sign with fewer panels sends\n");
  const struct display_map_entry three[] = {
      {"G1", '0', 0},
      {"G2", '0', 2},
      {"X92", '1', 5},
  };
  display_map_set(three, 3);
  check("synced after the first accepted mapping", display_map_synced(), 1);

  display_map_get(entries, &count);
  check("count is the mapping's, not the capacity", (int)count, 3);
  check("route survives the round trip", strcmp(entries[1].route, "G2") == 0, 1);
  check("position survives the round trip", entries[2].position, 5);
  check("direction survives the round trip", entries[2].direction, '1');

  printf("\nover-long mappings clamp rather than overrun\n");
#define OVER_CAPACITY (DISPLAY_BOX_CAPACITY + 2)
  struct display_map_entry many[OVER_CAPACITY];
  for (size_t i = 0; i < OVER_CAPACITY; i++) {
    snprintf(many[i].route, sizeof(many[i].route), "R%u", (unsigned)i);
    many[i].direction = '0';
    many[i].position = (uint8_t)(i % DISPLAY_BOX_CAPACITY);
  }
  display_map_set(many, OVER_CAPACITY);
  display_map_get(entries, &count);
  check("clamped to the box capacity", (int)count, DISPLAY_BOX_CAPACITY);

  printf("\nthe flag does not go back\n");
  check("still synced", display_map_synced(), 1);

  printf("\n%s (%d failing assertion(s))\n", failures ? "FAILURES" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
