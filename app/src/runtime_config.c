#include "runtime_config.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(runtime_config);

/* Same locking contract as stop_id.c: written by the pigeon client thread
 * (setters below), read by main's NTP/Swiftly paths (getters). The lock
 * covers the string copies only -- the int knobs are single aligned words
 * with one writer, so plain reads can't tear on this core. */
static K_MUTEX_DEFINE(runtime_config_lock);

static char ntp_primary[RUNTIME_NTP_SERVER_MAX_LEN] = CONFIG_PRIMARY_NTP_SERVER;
static char ntp_fallback[RUNTIME_NTP_SERVER_MAX_LEN] = CONFIG_FALLBACK_NTP_SERVER;
static int ntp_timeout_ms = CONFIG_NTP_REQUEST_TIMEOUT_MS;
static int ntp_retry_count = CONFIG_NTP_FETCH_RETRY_COUNT;
static int http_retry_count = CONFIG_HTTP_REQUEST_RETRY_COUNT;

void runtime_config_ntp_servers_get(
    char* primary, size_t primary_len, char* fallback, size_t fallback_len
) {
  k_mutex_lock(&runtime_config_lock, K_FOREVER);
  snprintk(primary, primary_len, "%s", ntp_primary);
  snprintk(fallback, fallback_len, "%s", ntp_fallback);
  k_mutex_unlock(&runtime_config_lock);
}

void runtime_config_ntp_servers_set(const char* primary, const char* fallback) {
  k_mutex_lock(&runtime_config_lock, K_FOREVER);

  if (primary && *primary) {
    snprintk(ntp_primary, sizeof(ntp_primary), "%s", primary);
  }

  if (fallback && *fallback) {
    snprintk(ntp_fallback, sizeof(ntp_fallback), "%s", fallback);
  }

  k_mutex_unlock(&runtime_config_lock);
}

int runtime_config_ntp_timeout_ms(void) { return ntp_timeout_ms; }

void runtime_config_ntp_timeout_ms_set(int value) { ntp_timeout_ms = value; }

int runtime_config_ntp_retry_count(void) { return ntp_retry_count; }

void runtime_config_ntp_retry_count_set(int value) { ntp_retry_count = value; }

int runtime_config_http_retry_count(void) { return http_retry_count; }

void runtime_config_http_retry_count_set(int value) { http_retry_count = value; }
