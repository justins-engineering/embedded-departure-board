#include "runtime_config.h"

/* Written by the pigeon client thread (setter below), read by main's
 * Swiftly retry path (getter): a single aligned word with one writer, so
 * plain reads cannot tear on this core and no lock is needed. */
static int http_retry_count = CONFIG_HTTP_REQUEST_RETRY_COUNT;

int runtime_config_http_retry_count(void) { return http_retry_count; }

void runtime_config_http_retry_count_set(int value) { http_retry_count = value; }
