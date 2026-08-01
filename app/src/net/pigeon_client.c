/** @headerfile pigeon_client.h */
#include "pigeon_client.h"

#include <nrf_modem_at.h>
#include <pigeon.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "net/lte_manager.h"
#include "runtime_config.h"
#include "stop_id.h"
#include "update_stop.h"

LOG_MODULE_REGISTER(pigeon_client);

/* All PidgeIoT I/O runs on this dedicated thread, never on main: a pigeon
 * transaction can legitimately block for tens of seconds (modem-internal
 * TCP/TLS timeouts against an unreachable api.pidgeiot.com), and main's
 * loop is what feeds the 60s hardware watchdog and drives the display.
 * Priority 10 (same as pigeon_ws.c's worker precedent) is BELOW main's 0:
 * the sign's day job preempts pigeon work at any instant, and pigeon only
 * runs while main sleeps between loop passes (see main.c's k_sleep note).
 * The one shared mutable state is current_stop_id, guarded inside
 * stop_id.c. */
#define PIGEON_CLIENT_THREAD_PRIORITY 10

static K_THREAD_STACK_DEFINE(pigeon_thread_stack, CONFIG_PIGEON_CLIENT_THREAD_STACK_SIZE);
static struct k_thread pigeon_thread_data;

static K_SEM_DEFINE(pigeon_poll_sem, 1, 1);

static void pigeon_poll_timeout_handler(struct k_timer* timer_id) {
  ARG_UNUSED(timer_id);
  (void)k_sem_give(&pigeon_poll_sem);
}

static K_TIMER_DEFINE(pigeon_poll_timer, pigeon_poll_timeout_handler, NULL);

/* Poll interval currently in force -- CONFIG default until a shadow
 * supplies its own telemetry_interval. Also the keep-last fallback seed
 * when a later target_config drops the key (a shadow that stops mentioning
 * telemetry_interval keeps the last applied value rather than snapping
 * back to the CONFIG default). Written on the pigeon thread only. */
static int applied_interval_s = CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS;

/* Failed-cycle backoff (LTE data budget): a cycle where NOTHING reached the
 * platform doubles the wait to 2x, then caps at 4x the poll interval --
 * bounded, and self-healing the moment one round trip succeeds. The poll
 * timer keeps its period; the thread just declines to touch the network
 * until the backoff window has passed. */
static unsigned int failed_cycles;
static int64_t next_attempt_uptime_ms;

/* Display refresh cadence currently in force -- runtime-overridable like
 * the knobs in runtime_config.c, but applied here (it drives
 * update_stop_timer directly) rather than read through a getter. Written
 * on the pigeon thread only. */
static int applied_update_stop_interval_s = CONFIG_UPDATE_STOP_FREQUENCY_SECONDS;

/* Hard bounds on shadow-supplied update_stop_interval: the loop that runs
 * update_stop() is also the loop that feeds the 60s hardware watchdog
 * (CONFIG_MAX_TIME_INACTIVE_BEFORE_RESET_MS -- a wdt_install_timeout()
 * window, fixed at boot, NOT shadow-tunable), and its feeds only happen on
 * update_stop passes. An interval pushed past ~45s would idle main
 * straight into a watchdog reset; a shadow must not be able to do that,
 * so out-of-range values clamp (and the clamped value is what gets
 * acked). The floor is Swiftly-API politeness. */
#define UPDATE_STOP_INTERVAL_MIN_S 5
#define UPDATE_STOP_INTERVAL_MAX_S 45

/* Sanity bounds for the remaining int knobs; same clamp-and-ack policy. */
#define NTP_TIMEOUT_MIN_MS 500
#define NTP_TIMEOUT_MAX_MS 30000
#define NTP_RETRY_MIN 1
#define NTP_RETRY_MAX 5
#define HTTP_RETRY_MIN 0
#define HTTP_RETRY_MAX 3

static int clamp_int(int value, int lo, int hi) {
  return (value < lo) ? lo : ((value > hi) ? hi : value);
}

/* Mirrors pigeon-examples' https_init sample's shadow.c convention: the app
 * owns target_config's meaning, pigeon only stores/forwards the raw JSON
 * text (see pigeon_shadow_doc in pigeon.h). "reboot" is a one-shot command,
 * deliberately excluded from the persisted current_config we report back so
 * it doesn't refire on every poll after being applied once. Everything else
 * here is a runtime-tunable operational knob whose Kconfig default is the
 * boot-time fallback (stop_id.h / runtime_config.h). */
struct target_config_wire {
  char stop_id[STOP_ID_MAX_LEN];
  int telemetry_interval;
  int update_stop_interval;
  char ntp_server_primary[RUNTIME_NTP_SERVER_MAX_LEN];
  char ntp_server_fallback[RUNTIME_NTP_SERVER_MAX_LEN];
  int ntp_timeout_ms;
  int ntp_retry_count;
  int http_retry_count;
  bool reboot;
};

static const struct json_obj_descr target_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, stop_id, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, update_stop_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_server_primary, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_server_fallback, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_timeout_ms, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_retry_count, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, http_retry_count, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, reboot, JSON_TOK_TRUE),
};

/* bit 0 = stop_id, bit 1 = telemetry_interval -- "reboot" (bit 2) is
 * deliberately optional to decode: a target_config with no reboot key at
 * all (the overwhelmingly common case) must not be treated as a parse
 * failure. stop_id is the one field this integration actually needs to be
 * useful, so it's the only one required for a target_config to count as
 * "applied" at all. */
#define TARGET_CONFIG_REQUIRED_BITS 0x1

static void apply_target_config(const char* target_config, int32_t target_version) {
  /* Defaults preserved for any field the shadow's JSON doesn't include --
   * json_obj_parse() only writes fields it actually decodes. Seeded from
   * the values currently in force (not the CONFIG defaults), so a shadow
   * that omits a key means "keep what you have". */
  struct target_config_wire cfg = {
      .telemetry_interval = applied_interval_s,
      .update_stop_interval = applied_update_stop_interval_s,
      .ntp_timeout_ms = runtime_config_ntp_timeout_ms(),
      .ntp_retry_count = runtime_config_ntp_retry_count(),
      .http_retry_count = runtime_config_http_retry_count(),
      .reboot = false,
  };
  stop_id_get(cfg.stop_id, sizeof(cfg.stop_id));
  runtime_config_ntp_servers_get(
      cfg.ntp_server_primary, sizeof(cfg.ntp_server_primary), cfg.ntp_server_fallback,
      sizeof(cfg.ntp_server_fallback)
  );

  int64_t decoded = json_obj_parse(
      (char*)target_config, strlen(target_config), target_config_descr,
      ARRAY_SIZE(target_config_descr), &cfg
  );

  if (decoded < 0 || (decoded & TARGET_CONFIG_REQUIRED_BITS) != TARGET_CONFIG_REQUIRED_BITS) {
    LOG_ERR("Shadow target_config missing/invalid stop_id (decoded=%lld); not applying", decoded);
    return;
  }

  /* The ack below embeds these decoded strings back into raw JSON without
   * an escaper -- a quote/backslash smuggled through a shadow value would
   * make every ack invalid (never converging, re-applying each poll), so
   * refuse such values outright rather than acking garbage. No real stop
   * ID or hostname contains either character. */
  if (strpbrk(cfg.stop_id, "\"\\") != NULL) {
    LOG_ERR("Shadow stop_id contains JSON-unsafe characters; not applying");
    return;
  }

  if (strpbrk(cfg.ntp_server_primary, "\"\\") != NULL ||
      strpbrk(cfg.ntp_server_fallback, "\"\\") != NULL) {
    LOG_ERR("Shadow NTP server contains JSON-unsafe characters; keeping current servers");
    runtime_config_ntp_servers_get(
        cfg.ntp_server_primary, sizeof(cfg.ntp_server_primary), cfg.ntp_server_fallback,
        sizeof(cfg.ntp_server_fallback)
    );
  }

  stop_id_set(cfg.stop_id);

  if (cfg.telemetry_interval > 0 && cfg.telemetry_interval != applied_interval_s) {
    LOG_INF("Shadow telemetry_interval: %ds", cfg.telemetry_interval);
    applied_interval_s = cfg.telemetry_interval;
    k_timer_start(
        &pigeon_poll_timer, K_SECONDS(cfg.telemetry_interval), K_SECONDS(cfg.telemetry_interval)
    );
  }

  cfg.update_stop_interval =
      clamp_int(cfg.update_stop_interval, UPDATE_STOP_INTERVAL_MIN_S, UPDATE_STOP_INTERVAL_MAX_S);
  if (cfg.update_stop_interval != applied_update_stop_interval_s) {
    LOG_INF("Shadow update_stop_interval: %ds", cfg.update_stop_interval);
    applied_update_stop_interval_s = cfg.update_stop_interval;
    k_timer_start(
        &update_stop_timer, K_SECONDS(cfg.update_stop_interval), K_SECONDS(cfg.update_stop_interval)
    );
  }

  cfg.ntp_timeout_ms = clamp_int(cfg.ntp_timeout_ms, NTP_TIMEOUT_MIN_MS, NTP_TIMEOUT_MAX_MS);
  cfg.ntp_retry_count = clamp_int(cfg.ntp_retry_count, NTP_RETRY_MIN, NTP_RETRY_MAX);
  cfg.http_retry_count = clamp_int(cfg.http_retry_count, HTTP_RETRY_MIN, HTTP_RETRY_MAX);
  runtime_config_ntp_servers_set(cfg.ntp_server_primary, cfg.ntp_server_fallback);
  runtime_config_ntp_timeout_ms_set(cfg.ntp_timeout_ms);
  runtime_config_ntp_retry_count_set(cfg.ntp_retry_count);
  runtime_config_http_retry_count_set(cfg.http_retry_count);

  /* Ack the config actually applied (minus "reboot", see this file's
   * struct doc comment) -- APPLIED, not requested: clamped values are
   * acked clamped, so the dashboard sees what the device really runs. */
  char current_config[CONFIG_PIGEON_SHADOW_CONFIG_MAX];

  snprintk(
      current_config, sizeof(current_config),
      "{\"stop_id\":\"%s\",\"telemetry_interval\":%d,\"update_stop_interval\":%d,"
      "\"ntp_server_primary\":\"%s\",\"ntp_server_fallback\":\"%s\",\"ntp_timeout_ms\":%d,"
      "\"ntp_retry_count\":%d,\"http_retry_count\":%d}",
      cfg.stop_id, cfg.telemetry_interval, cfg.update_stop_interval, cfg.ntp_server_primary,
      cfg.ntp_server_fallback, cfg.ntp_timeout_ms, cfg.ntp_retry_count, cfg.http_retry_count
  );
  int err = pigeon_shadow_report(target_version, current_config);

  if (err) {
    LOG_WRN(
        "pigeon_shadow_report failed: %d%s", err,
        cfg.reboot ? " -- deferring shadow-requested reboot until the ack lands" : ""
    );
  }

  /* Reboot only once the ack landed: an unacked reboot would refire on
   * every poll of an offline-ish platform (GET succeeding, POST failing),
   * turning one operator command into a reboot loop. Deferred, it fires
   * exactly once, on whichever later poll finally lands the ack. */
  if (cfg.reboot && err == 0) {
    LOG_WRN("Shadow requested reboot -- restarting now");
    lte_disconnect();
    sys_reboot(SYS_REBOOT_WARM);
  }
}

/* Read RSRP via AT+CESQ (+CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>,
 * 3GPP TS 27.007) rather than pulling in the modem_info library for just
 * this one value -- matches this codebase's existing hand-rolled style
 * (custom_http_client.c also talks sockets/AT-adjacent APIs directly rather
 * than through a heavier convenience layer). rsrp raw range is 0-97
 * (255 = not known/detectable); dBm = raw - 140 per the same spec.
 * @return dBm on success, INT32_MIN if unavailable/unparseable. */
static int32_t read_rsrp_dbm(void) {
  int rsrp_raw;
  int ret = nrf_modem_at_scanf("AT+CESQ", "+CESQ: %*d,%*d,%*d,%*d,%*d,%d", &rsrp_raw);

  if (ret != 1 || rsrp_raw > 97) {
    return INT32_MIN;
  }

  return rsrp_raw - 140;
}

/* Batched via pigeon_telemetry_set()/pigeon_telemetry_flush() (pigeon task
 * #64): all keys stage into the library's latest-value-per-key pending
 * store (CONFIG_PIGEON_TELEMETRY_MAX_KEYS, default 8 -- plenty for these 4)
 * and go out as ONE flat-JSON POST per poll cycle instead of one POST per
 * key -- a quarter of the previous LTE request cost. Set results are
 * ignorable by construction here (4 short keys can't hit the -ENOMEM/
 * -ENOSPC limits); a failed flush keeps its keys queued (clear-on-success),
 * and since every key is re-set with a fresh value each cycle, a transient
 * failure just means this cycle's gauges ride the next report.
 * @return 0 if the flush reached the platform, negative error otherwise. */
static int report_telemetry(void) {
  char val[16];

  snprintk(val, sizeof(val), "%lld", k_uptime_get() / 1000);
  (void)pigeon_telemetry_set("uptime_s", val);

  int32_t rsrp_dbm = read_rsrp_dbm();

  if (rsrp_dbm != INT32_MIN) {
    snprintk(val, sizeof(val), "%d", rsrp_dbm);
    (void)pigeon_telemetry_set("rsrp_dbm", val);
  }

  snprintk(val, sizeof(val), "%u", swiftly_consecutive_failures());
  (void)pigeon_telemetry_set("swiftly_consecutive_failures", val);

  int age_s = swiftly_last_success_age_s();

  if (age_s >= 0) {
    snprintk(val, sizeof(val), "%d", age_s);
    (void)pigeon_telemetry_set("swiftly_last_success_age_s", val);
  }

  int err = pigeon_telemetry_flush();

  if (err) {
    LOG_WRN("pigeon_telemetry_flush failed: %d (keys stay queued)", err);
  }

  return err;
}

/* One shadow sync + telemetry report cycle.
 * @return 0 if at least one request completed a round trip to the
 * platform this cycle (i.e. PidgeIoT is reachable), negative otherwise. */
static int pigeon_client_cycle(void) {
  struct pigeon_shadow_doc shadow;
  int err = pigeon_shadow_get(&shadow);

  if (err) {
    LOG_ERR("pigeon_shadow_get failed: %d", err);
  } else if (shadow.target_version != shadow.current_version) {
    apply_target_config(shadow.target_config, shadow.target_version);
  }

  int telemetry_err = report_telemetry();

  return (err == 0 || telemetry_err == 0) ? 0 : -EIO;
}

static void pigeon_thread_fn(void* p1, void* p2, void* p3) {
  ARG_UNUSED(p1);
  ARG_UNUSED(p2);
  ARG_UNUSED(p3);

  while (true) {
    (void)k_sem_take(&pigeon_poll_sem, K_FOREVER);

    if (failed_cycles > 0 && k_uptime_get() < next_attempt_uptime_ms) {
      continue;
    }

    if (pigeon_client_cycle() == 0) {
      failed_cycles = 0;
    } else {
      failed_cycles++;

      unsigned int mult = 1U << MIN(failed_cycles, 2U);

      next_attempt_uptime_ms = k_uptime_get() + ((int64_t)applied_interval_s * 1000 * mult);
      LOG_WRN(
          "PidgeIoT unreachable (%u consecutive cycle(s)); next attempt in ~%ds", failed_cycles,
          applied_interval_s * (int)mult
      );
    }
  }
}

void pigeon_client_init(void) {
  /* device_id is a local diagnostic label only (pigeon_init() just logs it;
   * the actual pigeon/Durable-Object identity dovecote cares about is
   * embedded in CONFIG_PIGEON_ENDPOINT's path, see pigeon.h). Deliberately
   * NOT current_stop_id/CONFIG_STOP_ID -- conflating "which physical device
   * this is" with "which stop it's currently showing" is exactly the
   * one-branch-per-stop anti-pattern this integration replaces. */
  const struct pigeon_config config = {
      .device_id = "embedded-departure-board",
      .connector = {.type = PIGEON_CONNECTOR_HTTPS},
  };

  int err = pigeon_init(&config);

  if (err) {
    LOG_ERR("pigeon_init failed: %d -- sign runs unmanaged this boot", err);
    return;
  }

  k_timer_start(&pigeon_poll_timer, K_SECONDS(applied_interval_s), K_SECONDS(applied_interval_s));

  /* pigeon_poll_sem starts at 1, so the thread syncs the shadow immediately
   * on start (picking up e.g. a stop_id set from the dashboard before this
   * boot) instead of waiting out a full first interval -- but on ITS stack,
   * never blocking main's boot path. */
  k_tid_t tid = k_thread_create(
      &pigeon_thread_data, pigeon_thread_stack, K_THREAD_STACK_SIZEOF(pigeon_thread_stack),
      pigeon_thread_fn, NULL, NULL, NULL, PIGEON_CLIENT_THREAD_PRIORITY, 0, K_NO_WAIT
  );

  (void)k_thread_name_set(tid, "pigeon_client");
}
