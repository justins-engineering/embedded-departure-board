#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H
#include <stddef.h>
#include <stdint.h>
/* Kept as a real static assertion rather than a no-op: the invariants it
 * guards (buffer size vs jsmn's int16 offsets, jsmntok_t packing) must hold
 * on the host build too or the harness is testing a different parser. */
#define BUILD_ASSERT(EXPR, ...) _Static_assert(EXPR, #EXPR)

/* Left incomplete on purpose: update_stop.h declares a timer and a
 * semaphore this harness never touches, and naming them is all that is
 * needed to pull in the real DISPLAY_BOXES table alongside them. */
struct k_timer;
struct k_sem;
#endif
