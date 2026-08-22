#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H

#include <stdio.h>

/* Single-threaded harness: the mutex collapses to a no-op, so these tests
 * exercise the seed-vs-synced state machine, not the locking. Keep it that
 * way. A pthreads mutex here would buy a race test far too weak to trust
 * while complicating every case that actually matters. */
struct k_mutex {
  int unused;
};

#define K_MUTEX_DEFINE(name) struct k_mutex name
#define K_FOREVER 0

static inline int k_mutex_lock(struct k_mutex* mutex, int timeout) {
  (void)mutex;
  (void)timeout;
  return 0;
}

static inline int k_mutex_unlock(struct k_mutex* mutex) {
  (void)mutex;
  return 0;
}

/* Same trim-and-NUL-terminate contract on the host. */
#define snprintk snprintf

#endif
