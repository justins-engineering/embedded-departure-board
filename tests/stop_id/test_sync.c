/* The boot-race gate's seam: current_stop_id starts as the compiled-in
 * CONFIG_STOP_ID seed with stop_id_synced() false, and only an accepted
 * runtime value flips it true. Compiles the real app/src/stop_id.c against
 * stub headers, same idiom as ../parse_swiftly. The flag is monotonic, so
 * case order matters: everything asserting "still unsynced" has to run
 * before the first accepted set. */
#include <stdio.h>
#include <string.h>

#include "stop_id.h"

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
  char buf[STOP_ID_MAX_LEN];

  printf("boot state\n");
  stop_id_get(buf, sizeof(buf));
  check("value is the CONFIG_STOP_ID seed", strcmp(buf, CONFIG_STOP_ID) == 0, 1);
  check("seed alone does not count as synced", stop_id_synced(), 0);

  /* stop_id_set()'s documented reject cases must leave the gate closed:
   * a shadow that failed to supply a usable stop ID has not answered the
   * "which stop" question, however recently it was heard from. */
  printf("rejected values do not open the gate\n");
  stop_id_set(NULL);
  check("NULL rejected, still unsynced", stop_id_synced(), 0);
  stop_id_set("");
  check("empty rejected, still unsynced", stop_id_synced(), 0);
  stop_id_get(buf, sizeof(buf));
  check("value untouched by the rejects", strcmp(buf, CONFIG_STOP_ID) == 0, 1);

  /* The bench case: the shadow's stop_id can equal the compiled-in seed,
   * and confirming the seed is still an authoritative answer. A gate that
   * only opened on a CHANGE would leave such a sign dark forever. */
  printf("a runtime value equal to the seed still counts\n");
  stop_id_set(CONFIG_STOP_ID);
  check("synced after applying the identical value", stop_id_synced(), 1);
  stop_id_get(buf, sizeof(buf));
  check("value unchanged", strcmp(buf, CONFIG_STOP_ID) == 0, 1);

  printf("a different stop takes effect\n");
  stop_id_set("1670");
  stop_id_get(buf, sizeof(buf));
  check("value updated", strcmp(buf, "1670") == 0, 1);

  /* Truncate-not-reject is stop_id.h's documented policy: a wrong-looking
   * stop on the sign is debuggable, a silently ignored update is not. */
  printf("overlong ids truncate rather than reject\n");
  stop_id_set("0123456789abcdefXYZ");
  stop_id_get(buf, sizeof(buf));
  check("truncated to STOP_ID_MAX_LEN - 1 chars", strcmp(buf, "0123456789abcde") == 0, 1);

  if (failures) {
    printf("%d case(s) failed\n", failures);
    return 1;
  }
  return 0;
}
