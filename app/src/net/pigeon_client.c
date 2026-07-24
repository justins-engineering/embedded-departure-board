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
#include "stop_id.h"
#include "update_stop.h"

LOG_MODULE_REGISTER(pigeon_client);

K_SEM_DEFINE(pigeon_poll_sem, 0, 1);

static void pigeon_poll_timeout_handler(struct k_timer *timer_id) {
  ARG_UNUSED(timer_id);
  (void)k_sem_give(&pigeon_poll_sem);
}

K_TIMER_DEFINE(pigeon_poll_timer, pigeon_poll_timeout_handler, NULL);

/* Mirrors pigeon-examples' https_init sample's shadow.c convention: the app
 * owns target_config's meaning, pigeon only stores/forwards the raw JSON
 * text (see pigeon_shadow_doc in pigeon.h). "reboot" is a one-shot command,
 * deliberately excluded from the persisted current_config we report back so
 * it doesn't refire on every poll after being applied once. */
struct target_config_wire {
  char stop_id[STOP_ID_MAX_LEN];
  int telemetry_interval;
  bool reboot;
};

static const struct json_obj_descr target_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, stop_id, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, reboot, JSON_TOK_TRUE),
};

/* bit 0 = stop_id, bit 1 = telemetry_interval -- "reboot" (bit 2) is
 * deliberately optional to decode: a target_config with no reboot key at
 * all (the overwhelmingly common case) must not be treated as a parse
 * failure. stop_id is the one field this integration actually needs to be
 * useful, so it's the only one required for a target_config to count as
 * "applied" at all. */
#define TARGET_CONFIG_REQUIRED_BITS 0x1

static void apply_target_config(const char *target_config, int32_t target_version) {
  /* Defaults preserved for any field the shadow's JSON doesn't include --
   * json_obj_parse() only writes fields it actually decodes. */
  struct target_config_wire cfg = {
      .telemetry_interval = CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS,
      .reboot = false,
  };
  snprintk(cfg.stop_id, sizeof(cfg.stop_id), "%s", current_stop_id);

  int64_t decoded = json_obj_parse(
      (char *)target_config, strlen(target_config), target_config_descr,
      ARRAY_SIZE(target_config_descr), &cfg
  );

  if (decoded < 0 || (decoded & TARGET_CONFIG_REQUIRED_BITS) != TARGET_CONFIG_REQUIRED_BITS) {
    LOG_ERR("Shadow target_config missing/invalid stop_id (decoded=%lld); not applying", decoded);
    return;
  }

  stop_id_set(cfg.stop_id);

  if (cfg.telemetry_interval > 0 &&
      cfg.telemetry_interval != CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS) {
    LOG_INF("Shadow telemetry_interval: %ds", cfg.telemetry_interval);
    k_timer_start(
        &pigeon_poll_timer, K_SECONDS(cfg.telemetry_interval), K_SECONDS(cfg.telemetry_interval)
    );
  }

  /* Ack the config actually applied (minus "reboot", see this file's
   * struct doc comment) before possibly rebooting, so the shadow converges
   * even if this device never comes back up cleanly. */
  char current_config[STOP_ID_MAX_LEN + 64];

  snprintk(
      current_config, sizeof(current_config), "{\"stop_id\":\"%s\",\"telemetry_interval\":%d}",
      cfg.stop_id, cfg.telemetry_interval
  );
  int err = pigeon_shadow_report(target_version, current_config);

  if (err) {
    LOG_WRN("pigeon_shadow_report failed: %d", err);
  }

  if (cfg.reboot) {
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

/* pigeon_set_shadow_param()/pigeon_shadow_flush() hold only a single
 * pending key/val slot (see pigeon_core.c) -- there is no batch-report
 * API, so each telemetry key here is its own flush (its own HTTPS POST).
 * Four short requests per CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS is a
 * deliberately modest LTE data cost for a mains-powered sign with no
 * power budget to speak of, but cellular data still isn't free. */
static void report_telemetry(void) {
  char val[16];

  snprintk(val, sizeof(val), "%lld", k_uptime_get() / 1000);
  (void)pigeon_set_shadow_param("uptime_s", val);
  (void)pigeon_shadow_flush();

  int32_t rsrp_dbm = read_rsrp_dbm();

  if (rsrp_dbm != INT32_MIN) {
    snprintk(val, sizeof(val), "%d", rsrp_dbm);
    (void)pigeon_set_shadow_param("rsrp_dbm", val);
    (void)pigeon_shadow_flush();
  }

  snprintk(val, sizeof(val), "%u", swiftly_consecutive_failures());
  (void)pigeon_set_shadow_param("swiftly_consecutive_failures", val);
  (void)pigeon_shadow_flush();

  int age_s = swiftly_last_success_age_s();

  if (age_s >= 0) {
    snprintk(val, sizeof(val), "%d", age_s);
    (void)pigeon_set_shadow_param("swiftly_last_success_age_s", val);
    (void)pigeon_shadow_flush();
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
    LOG_ERR("pigeon_init failed: %d", err);
    return;
  }

  k_timer_start(
      &pigeon_poll_timer, K_SECONDS(CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS),
      K_SECONDS(CONFIG_PIGEON_CLIENT_POLL_INTERVAL_SECONDS)
  );

  /* Apply whatever shadow config already exists before the first
   * telemetry report -- an immediate poll rather than waiting a full
   * interval for the first sync after a fresh boot/reflash. */
  pigeon_client_poll();
}

void pigeon_client_poll(void) {
  struct pigeon_shadow_doc shadow;
  int err = pigeon_shadow_get(&shadow);

  if (err) {
    LOG_ERR("pigeon_shadow_get failed: %d", err);
  } else if (shadow.target_version != shadow.current_version) {
    apply_target_config(shadow.target_config, shadow.target_version);
  }

  report_telemetry();
}
