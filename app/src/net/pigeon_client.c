/** @headerfile pigeon_client.h */
#include "pigeon_client.h"

#include <nrf_modem_at.h>
#include <pigeon.h>
#include <stdio.h>
#include <string.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_PIGEON_FOTA)
#include <zephyr/settings/settings.h>
#endif

#include "display_map.h"
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

#if defined(CONFIG_PIGEON_FOTA)
/* True while pigeon_fota_apply() is streaming chunks (set/cleared on the
 * pigeon thread, read by main -- see pigeon_client_fota_active()). Left
 * set through the staged-swap reboot on purpose: main's reset policy
 * stays suppressed for the seconds between staging and restart.
 *
 * BOUNDED: the suppression self-expires after
 * FOTA_SUPPRESSION_CEILING_MS even if the flag is still set -- a wedged
 * download must never trade "reboots kill downloads" for "a wedged
 * download disables the sign's self-recovery forever". 60 min, sized
 * from MEASURED reality, not the model: the first live 0.13.2 attempt
 * ran ~2.9s/chunk under link contention (~46 min for a full 949-chunk
 * image), so the original 40-min ceiling would have expired mid-download
 * on an otherwise-succeeding attempt and let a late Swiftly hard-fail
 * reboot it. 60 covers the measured worst case with margin while still
 * bounding a genuinely wedged download. */
#define FOTA_SUPPRESSION_CEILING_MS (60 * 60 * 1000)

static atomic_t fota_in_progress;
static int64_t fota_active_since_ms;
#endif

bool pigeon_client_fota_active(void) {
#if defined(CONFIG_PIGEON_FOTA)
  if (atomic_get(&fota_in_progress) != 1) {
    return false;
  }

  return (k_uptime_get() - fota_active_since_ms) < FOTA_SUPPRESSION_CEILING_MS;
#else
  return false;
#endif
}

/* Display refresh cadence currently in force -- runtime-overridable like
 * the knobs in runtime_config.c, but applied here (it drives
 * update_stop_timer directly) rather than read through a getter. Written
 * on the pigeon thread only. */
static int applied_update_stop_interval_s = CONFIG_UPDATE_STOP_FREQUENCY_SECONDS;

int pigeon_client_update_stop_interval_s(void) {
  return applied_update_stop_interval_s;
}

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
/* One entry of the shadow's "displays" array -- 1-char keys keep a full
 * 6-entry layout compact against CONFIG_PIGEON_SHADOW_CONFIG_MAX:
 * {"r":"<route>","d":"<direction char>","p":<box position>}. */
struct display_wire {
  char r[DISPLAY_MAP_ROUTE_LEN + 3];
  char d[2];
  int p;
};

static const struct json_obj_descr display_entry_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct display_wire, r, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct display_wire, d, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct display_wire, p, JSON_TOK_NUMBER),
};

struct target_config_wire {
  char stop_id[STOP_ID_MAX_LEN];
  int telemetry_interval;
  int update_stop_interval;
  char ntp_server_primary[RUNTIME_NTP_SERVER_MAX_LEN];
  char ntp_server_fallback[RUNTIME_NTP_SERVER_MAX_LEN];
  int ntp_timeout_ms;
  int ntp_retry_count;
  int http_retry_count;
  struct display_wire displays[CONFIG_NUMBER_OF_DISPLAY_BOXES];
  size_t displays_len;
  bool reboot;
#if defined(CONFIG_PIGEON_FOTA)
  /* The shadow's optional "firmware" object (version/size/sha256, see
   * capsules::FirmwareTarget) -- decoded straight into the same struct
   * pigeon_fota_update_available()/pigeon_fota_apply() consume. */
  struct pigeon_fota_info firmware;
#endif
};

#if defined(CONFIG_PIGEON_FOTA)
static const struct json_obj_descr firmware_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, version, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, size, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct pigeon_fota_info, sha256, JSON_TOK_STRING_BUF),
};
#endif

static const struct json_obj_descr target_config_descr[] = {
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, stop_id, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, telemetry_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, update_stop_interval, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_server_primary, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_server_fallback, JSON_TOK_STRING_BUF),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_timeout_ms, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, ntp_retry_count, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, http_retry_count, JSON_TOK_NUMBER),
    JSON_OBJ_DESCR_OBJ_ARRAY(
        struct target_config_wire, displays, CONFIG_NUMBER_OF_DISPLAY_BOXES, displays_len,
        display_entry_descr, ARRAY_SIZE(display_entry_descr)
    ),
    JSON_OBJ_DESCR_PRIM(struct target_config_wire, reboot, JSON_TOK_TRUE),
#if defined(CONFIG_PIGEON_FOTA)
    /* Keep last: TARGET_CONFIG_FW_BIT below assumes it. */
    JSON_OBJ_DESCR_OBJECT(struct target_config_wire, firmware, firmware_descr),
#endif
};

#if defined(CONFIG_PIGEON_FOTA)
#define TARGET_CONFIG_FW_BIT BIT(ARRAY_SIZE(target_config_descr) - 1)
#endif

/* bit 0 = stop_id, bit 1 = telemetry_interval -- "reboot" (bit 2) is
 * deliberately optional to decode: a target_config with no reboot key at
 * all (the overwhelmingly common case) must not be treated as a parse
 * failure. stop_id is the one field this integration actually needs to be
 * useful, so it's the only one required for a target_config to count as
 * "applied" at all. */
#define TARGET_CONFIG_REQUIRED_BITS 0x1

/* Descriptor-order bit of the "displays" array (KEEP IN SYNC with
 * target_config_descr: stop_id=0 ... http_retry_count=7, displays=8). */
#define TARGET_CONFIG_DISPLAYS_BIT BIT(8)

#if defined(CONFIG_PIGEON_FOTA)

/* Bounded FOTA retries, persistent across reboots: the attempt counter is
 * bumped and saved to settings/NVS BEFORE each download, so even the worst
 * failure shape -- an image that downloads and verifies fine, test-swaps,
 * then boot-loops until MCUboot reverts to this build -- burns its budget
 * and stops, instead of re-downloading a few hundred KB over LTE every
 * poll interval forever. The counter clears once a matching version is
 * seen actually running, or when the operator pushes a different target
 * version. In-RAM holdoff spaces retries of transient (transport/verify)
 * failures within one boot. */
#define FOTA_MAX_ATTEMPTS_PER_VERSION 3
#define FOTA_RETRY_HOLDOFF_MS (30 * 60 * 1000)

struct fota_attempt_record {
  char version[PIGEON_FOTA_VERSION_MAX];
  uint8_t count;
};

static struct fota_attempt_record fota_attempts;
static int64_t fota_holdoff_until_ms;

static int fota_settings_set(const char* name, size_t len, settings_read_cb read_cb, void* cb_arg) {
  const char* next;

  if (settings_name_steq(name, "attempts", &next) && !next && len == sizeof(fota_attempts)) {
    (void)read_cb(cb_arg, &fota_attempts, sizeof(fota_attempts));
  }

  return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(edb_fota, "edb/fota", NULL, fota_settings_set, NULL, NULL);

enum fw_action {
  FW_ACK_PLAIN,   /* no valid firmware target: ack without a firmware key */
  FW_ACK_RUNNING, /* already running the target: ack echoes the firmware key */
  FW_NO_ACK,      /* download pending/failed/held off: leave unconverged */
  FW_REBOOT,      /* staged + scheduled: reboot into the test swap */
};

static enum fw_action handle_firmware_target(const struct pigeon_fota_info* info) {
  /* The strpbrk arm keeps the ack's firmware echo JSON-safe, same policy
   * as the stop_id/NTP strings in apply_target_config(). */
  if (info->version[0] == '\0' || info->size <= 0 ||
      strlen(info->sha256) != PIGEON_FOTA_SHA256_HEX_LEN ||
      strpbrk(info->version, "\"\\") != NULL || strpbrk(info->sha256, "\"\\") != NULL) {
    LOG_ERR("Shadow firmware target malformed (version/size/sha256); ignoring it");
    return FW_ACK_PLAIN;
  }

  if (!pigeon_fota_update_available(info)) {
    /* This build IS the target: a fresh arrival converges here, on its
     * first shadow poll after the swap -- the ack is deliberately sent by
     * the NEW image, never by the old one pre-reboot, so a reverted swap
     * leaves the shadow visibly unconverged instead of lying. */
    if (strcmp(fota_attempts.version, info->version) == 0 && fota_attempts.count > 0) {
      fota_attempts.count = 0;
      (void)settings_save_one("edb/fota/attempts", &fota_attempts, sizeof(fota_attempts));
    }
    return FW_ACK_RUNNING;
  }

  if (strcmp(fota_attempts.version, info->version) == 0 &&
      fota_attempts.count >= FOTA_MAX_ATTEMPTS_PER_VERSION) {
    LOG_WRN(
        "FOTA: %s already attempted %u times (incl. across reboots); refusing until the "
        "shadow targets a different version",
        info->version, fota_attempts.count
    );
    return FW_NO_ACK;
  }

  if (k_uptime_get() < fota_holdoff_until_ms) {
    return FW_NO_ACK;
  }

  if (strcmp(fota_attempts.version, info->version) != 0) {
    snprintk(fota_attempts.version, sizeof(fota_attempts.version), "%s", info->version);
    fota_attempts.count = 0;
  }
  fota_attempts.count++;
  (void)settings_save_one("edb/fota/attempts", &fota_attempts, sizeof(fota_attempts));

  LOG_WRN(
      "FOTA: downloading %s (%d bytes, attempt %u/%u)", info->version, info->size,
      fota_attempts.count, FOTA_MAX_ATTEMPTS_PER_VERSION
  );

  /* Flag main BEFORE the first chunk: the download saturates LTE-M enough
   * that the Swiftly fetch can fail both its tries, and main's reset-on-
   * fetch-failure policy would otherwise reboot the board mid-download --
   * the exact interaction that burned all three attempts of the first
   * live OTA test (2026-08-02). See main.c's suppression check. */
  fota_active_since_ms = k_uptime_get();
  atomic_set(&fota_in_progress, 1);

  int err = pigeon_fota_apply(info);

  if (err) {
    atomic_set(&fota_in_progress, 0);
    LOG_ERR(
        "FOTA: apply failed: %d (next attempt in >=%d min)", err, FOTA_RETRY_HOLDOFF_MS / 60000
    );
    fota_holdoff_until_ms = k_uptime_get() + FOTA_RETRY_HOLDOFF_MS;
    return FW_NO_ACK;
  }

  return FW_REBOOT;
}

#endif /* CONFIG_PIGEON_FOTA */

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

  /* Seed the displays array from the mapping currently in force, so an
   * omitted "displays" key keeps it AND the ack always reports the
   * effective layout (the whole point of the as-applied ack). */
  struct display_map_entry cur_map[CONFIG_NUMBER_OF_DISPLAY_BOXES];
  size_t cur_count = 0;

  display_map_get(cur_map, &cur_count);
  cfg.displays_len = cur_count;
  for (size_t i = 0; i < cur_count; i++) {
    snprintk(cfg.displays[i].r, sizeof(cfg.displays[i].r), "%s", cur_map[i].route);
    cfg.displays[i].d[0] = cur_map[i].direction;
    cfg.displays[i].d[1] = '\0';
    cfg.displays[i].p = cur_map[i].position;
  }

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

  /* "displays": whole-array replacement, never a per-entry merge (the
   * shadow carries the complete layout for a stop). Any invalid entry
   * rejects the WHOLE array -- half a layout is worse than the old one --
   * and the ack then echoes the unchanged effective mapping. */
  if ((decoded & TARGET_CONFIG_DISPLAYS_BIT) != 0) {
    struct display_map_entry new_map[CONFIG_NUMBER_OF_DISPLAY_BOXES];
    bool displays_ok = (cfg.displays_len > 0);

    for (size_t i = 0; displays_ok && i < cfg.displays_len; i++) {
      const struct display_wire* w = &cfg.displays[i];

      if (w->r[0] == '\0' || strlen(w->r) >= DISPLAY_MAP_ROUTE_LEN ||
          strpbrk(w->r, "\"\\") != NULL || w->d[0] == '\0' || w->d[1] != '\0' ||
          strpbrk(w->d, "\"\\") != NULL || w->p < 0 || w->p >= CONFIG_NUMBER_OF_DISPLAY_BOXES) {
        displays_ok = false;
        break;
      }

      snprintk(new_map[i].route, sizeof(new_map[i].route), "%s", w->r);
      new_map[i].direction = w->d[0];
      new_map[i].position = (uint8_t)w->p;
    }

    if (displays_ok) {
      display_map_set(new_map, cfg.displays_len);
      LOG_INF("Shadow displays: %u entries applied", (unsigned)cfg.displays_len);
    } else {
      LOG_ERR("Shadow displays array invalid; keeping current mapping");
      cfg.displays_len = cur_count;
      for (size_t i = 0; i < cur_count; i++) {
        snprintk(cfg.displays[i].r, sizeof(cfg.displays[i].r), "%s", cur_map[i].route);
        cfg.displays[i].d[0] = cur_map[i].direction;
        cfg.displays[i].d[1] = '\0';
        cfg.displays[i].p = cur_map[i].position;
      }
    }
  }

#if defined(CONFIG_PIGEON_FOTA)
  bool ack_firmware = false;

  if ((decoded & TARGET_CONFIG_FW_BIT) != 0) {
    switch (handle_firmware_target(&cfg.firmware)) {
      case FW_ACK_RUNNING:
        ack_firmware = true;
        break;
      case FW_NO_ACK:
        /* Leave the shadow unconverged on purpose: the runtime knobs
         * above are applied (idempotent, they'll re-apply next poll),
         * but acking now would tell the platform a firmware target was
         * reached when it wasn't. */
        LOG_WRN("FOTA pending/held off -- skipping shadow ack this poll");
        return;
      case FW_REBOOT:
        /* Staged and scheduled. No ack from THIS image (see
         * handle_firmware_target) -- tear down gracefully and boot the
         * test swap; the new image acks once it proves out. */
        LOG_WRN("FOTA: image staged -- rebooting into MCUboot test swap");
        lte_disconnect();
        sys_reboot(SYS_REBOOT_WARM);
        break;
      case FW_ACK_PLAIN:
      default:
        break;
    }
  }
#endif /* CONFIG_PIGEON_FOTA */

  /* Ack the config actually applied (minus "reboot", see this file's
   * struct doc comment) -- APPLIED, not requested: clamped values are
   * acked clamped, so the dashboard sees what the device really runs. */
  char current_config[CONFIG_PIGEON_SHADOW_CONFIG_MAX];
  size_t ack_len = snprintk(
      current_config, sizeof(current_config),
      "{\"stop_id\":\"%s\",\"telemetry_interval\":%d,\"update_stop_interval\":%d,"
      "\"ntp_server_primary\":\"%s\",\"ntp_server_fallback\":\"%s\",\"ntp_timeout_ms\":%d,"
      "\"ntp_retry_count\":%d,\"http_retry_count\":%d",
      cfg.stop_id, cfg.telemetry_interval, cfg.update_stop_interval, cfg.ntp_server_primary,
      cfg.ntp_server_fallback, cfg.ntp_timeout_ms, cfg.ntp_retry_count, cfg.http_retry_count
  );

  /* Effective route->display layout, always echoed (see the seeding
   * comment above). */
  if (ack_len < sizeof(current_config)) {
    ack_len +=
        snprintk(current_config + ack_len, sizeof(current_config) - ack_len, ",\"displays\":[");
    for (size_t i = 0; i < cfg.displays_len && ack_len < sizeof(current_config); i++) {
      ack_len += snprintk(
          current_config + ack_len, sizeof(current_config) - ack_len,
          "%s{\"r\":\"%s\",\"d\":\"%s\",\"p\":%d}", (i > 0) ? "," : "", cfg.displays[i].r,
          cfg.displays[i].d, cfg.displays[i].p
      );
    }
    if (ack_len < sizeof(current_config)) {
      ack_len += snprintk(current_config + ack_len, sizeof(current_config) - ack_len, "]");
    }
  }

#if defined(CONFIG_PIGEON_FOTA)
  if (ack_firmware && ack_len < sizeof(current_config)) {
    ack_len += snprintk(
        current_config + ack_len, sizeof(current_config) - ack_len,
        ",\"firmware\":{\"version\":\"%s\",\"size\":%d,\"sha256\":\"%s\"}", cfg.firmware.version,
        cfg.firmware.size, cfg.firmware.sha256
    );
  }
#endif

  if (ack_len < sizeof(current_config) - 1) {
    current_config[ack_len] = '}';
    current_config[ack_len + 1] = '\0';
  } else {
    /* Truncated ack would be invalid JSON; CONFIG_PIGEON_SHADOW_CONFIG_MAX
     * is sized so this can't happen with in-range values -- treat it as a
     * bug rather than sending garbage. */
    LOG_ERR("Shadow ack overflowed its buffer (%u bytes); not reporting", (unsigned)ack_len);
    return;
  }

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

#if defined(CONFIG_PIGEON_FOTA)
  /* Restore the persisted FOTA attempt record (see fota_attempt_record's
   * doc) -- best-effort: a settings failure just means the counter
   * restarts, never that the sign doesn't boot. */
  int serr = settings_subsys_init();

  if (serr == 0) {
    serr = settings_load_subtree("edb/fota");
  }

  if (serr) {
    LOG_WRN("FOTA attempt-record load failed: %d (starting fresh)", serr);
  }
#endif

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
