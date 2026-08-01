#include "ntp.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "runtime_config.h"

LOG_MODULE_REGISTER(ntp);

#define SNTP_PORT "123"

struct sntp_time time_stamp;

static int ntp_request(const char* url, int timeout_ms) {
  int err;
  struct sntp_ctx ctx;

  struct zsock_addrinfo* addr_inf;
  static struct zsock_addrinfo hints = {.ai_socktype = SOCK_DGRAM, .ai_flags = AI_NUMERICSERV};

  err = zsock_getaddrinfo(url, SNTP_PORT, &hints, &addr_inf);
  if (err) {
    LOG_ERR("getaddrinfo() failed, %s", strerror(errno));
    return errno;
  }

  if (addr_inf->ai_family == AF_INET) {
    err = sntp_init(&ctx, addr_inf->ai_addr, sizeof(struct sockaddr_in));
  } else {
    err = sntp_init(&ctx, addr_inf->ai_addr, sizeof(struct sockaddr_in6));
  }

  if (err < 0) {
    LOG_ERR("Failed to init SNTP ctx: %d", err);
    goto end;
  }

  err = sntp_query(&ctx, (uint32_t)timeout_ms, &time_stamp);
  if (err) {
    LOG_ERR("SNTP request failed: %d", err);
  }

end:
  sntp_close(&ctx);
  return err;
}

int get_ntp_time(void) {
  /* -EINVAL survives only if the retry loop never runs -- can't happen
   * with the setter-side clamp (retry count >= 1), but the compiler's
   * analyzer can't see that. */
  int err = -EINVAL;

  /* Snapshotted per call: these are shadow-overridable (runtime_config.h),
   * and the pigeon client thread may retune them between calls -- never
   * mid-call. */
  char primary[RUNTIME_NTP_SERVER_MAX_LEN];
  char fallback[RUNTIME_NTP_SERVER_MAX_LEN];
  int timeout_ms = runtime_config_ntp_timeout_ms();
  int retry_count = runtime_config_ntp_retry_count();

  runtime_config_ntp_servers_get(primary, sizeof(primary), fallback, sizeof(fallback));

  /* Get sntp time */
  for (int rc = 0; rc < (retry_count * 2); rc++) {
    if (rc < retry_count) {
      err = ntp_request(primary, timeout_ms);
    } else {
      err = ntp_request(fallback, timeout_ms);
    }

    if (err && (rc == (retry_count * 2) - 1)) {
      LOG_ERR(
          "Failed to get time from all NTP pools! Err: %i\n Check your network "
          "connection.",
          err
      );
    } else if (err && (rc == retry_count - 1)) {
      LOG_WRN(
          "Unable to get time after %d tries from NTP pool %s. Err: %i\n Attempting to use "
          "fallback NTP pool...",
          retry_count, primary, err
      );
    } else if (err) {
      LOG_WRN("Failed to get time using SNTP, Err: %i. Retrying...", err);
    } else {
      break;
    }
  }
  return err;
}
