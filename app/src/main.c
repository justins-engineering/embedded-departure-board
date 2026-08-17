#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/types.h>

#include "display/display_switches.h"
#include "net/lte_manager.h"
#include "net/pigeon_client.h"
#include "real_time_counter.h"
#include "update_stop.h"
#include "watchdog_app.h"

#ifdef CONFIG_LIGHT_SENSOR
#include "display/pwm_leds.h"
#include "light_sensor.h"
#endif  // CONFIG_LIGHT_SENSOR

#ifdef CONFIG_BOOTLOADER_MCUBOOT
#include "net/fota.h"
#endif  // CONFIG_BOOTLOADER_MCUBOOT

LOG_MODULE_REGISTER(main);

/* Reads without clearing. pigeon_init() reads the same register to report
 * the boot's cause as telemetry and never clears it, on the principle that
 * whichever of two readers runs second must still see the same value --
 * and this one runs first. Clearing is main's job, once both have read;
 * see the call site. */
void log_reset_reason(void) {
  uint32_t cause;
  int err = hwinfo_get_reset_cause(&cause);

  if (err) {
    LOG_ERR("Failed to get reset cause. Err: %d", err);
  } else {
    LOG_INF("Reset Reason: ");
    if (cause == 0) {
      LOG_ERR("RESET_UNKNOWN");
      return;
    }
    if (cause == RESET_PIN) {
      LOG_WRN("RESET_PIN");
    }
    if (cause == RESET_SOFTWARE) {
      LOG_WRN("RESET_SOFTWARE");
    }
    if (cause == RESET_BROWNOUT) {
      LOG_WRN("RESET_BROWNOUT");
    }
    if (cause == RESET_POR) {
      LOG_WRN("RESET_POR");
    }
    if (cause == RESET_WATCHDOG) {
      LOG_WRN("RESET_WATCHDOG");
    }
    if (cause == RESET_DEBUG) {
      LOG_WRN("RESET_DEBUG");
    }
    if (cause == RESET_SECURITY) {
      LOG_WRN("RESET_SECURITY");
    }
    if (cause == RESET_LOW_POWER_WAKE) {
      LOG_WRN("RESET_LOW_POWER_WAKE");
    }
    if (cause == RESET_CPU_LOCKUP) {
      LOG_WRN("RESET_CPU_LOCKUP");
    }
    if (cause == RESET_PARITY) {
      LOG_WRN("RESET_PARITY");
    }
    if (cause == RESET_PLL) {
      LOG_WRN("RESET_PLL");
    }
    if (cause == RESET_CLOCK) {
      LOG_WRN("RESET_CLOCK");
    }
    if (cause == RESET_HARDWARE) {
      LOG_WRN("RESET_HARDWARE");
    }
    if (cause == RESET_USER) {
      LOG_WRN("RESET_USER");
    }
    if (cause == RESET_TEMPERATURE) {
      LOG_WRN("RESET_TEMPERATURE");
    }
  }
}

#ifdef CONFIG_LED_DISPLAY_TEST
#include "display/led_display.h"

int main(void) {
  int err = init_display_switches();
  if (err < 0) {
    LOG_ERR("Failed to initialize display switches. Err: %d", err);
    goto end;
  }

#ifdef CONFIG_LIGHT_SENSOR
  err = pwm_leds_test();
  if (err) {
    goto end;
  }
#endif  // CONFIG_LIGHT_SENSOR

  while (1) {
    err = led_test_patern();
    if (err) {
      goto end;
    }

    err = max_power_test();
    if (err) {
      goto end;
    }
  }

end:
  LOG_ERR("Reached end of main; waiting for manual reset.");
}

#else
int main(void) {
  int ret;
  int wdt_channel_id = -1;

  ret = init_display_switches();
  if (ret < 0) {
    LOG_ERR("Failed to initialize display switches. Err: %d", ret);
    goto reset;
  }

  // Set all displays off because the LEDs have memory
  for (size_t box = 0; box < CONFIG_NUMBER_OF_DISPLAY_BOXES; box++) {
    ret = display_off(box);
    if (ret < 0) {
      LOG_ERR("Failed to set display switch %d off.", box);
    }
  }

#ifdef CONFIG_LIGHT_SENSOR
  int lux = get_lux();
  if (lux < 0) {
    goto reset;
  }

  ret = pwm_leds_set((uint32_t)lux);
  if (ret) {
    goto reset;
  }
#endif  // CONFIG_LIGHT_SENSOR

  (void)log_reset_reason();

#ifdef CONFIG_BOOTLOADER_MCUBOOT
  (void)validate_image();
#endif

  wdt_channel_id = watchdog_init();
  if (wdt_channel_id < 0) {
    LOG_ERR("Failed to initialize watchdog. Err: %d", wdt_channel_id);
    goto reset;
  }

  ret = wdt_feed(wdt, wdt_channel_id);
  if (ret) {
    LOG_ERR("Failed to feed watchdog. Err: %d", ret);
    goto reset;
  }

  ret = lte_connect();
  if (ret) {
    goto reset;
  }

  if (k_sem_take(&lte_connected_sem, K_FOREVER) == 0) {
    ret = set_rtc_time();
    if (ret) {
      LOG_ERR("Failed to set rtc.");
      goto reset;
    }
  } else {
    LOG_ERR("Failed to take network_connected_sem.");
    goto reset;
  }
  k_sem_give(&lte_connected_sem);

  ret = wdt_feed(wdt, wdt_channel_id);
  if (ret) {
    LOG_ERR("Failed to feed watchdog. Err: %d", ret);
    goto reset;
  }

  // TODO: check for update or wait for update socket
  // (void)download_update();

  (void)k_timer_start(
      &update_stop_timer, K_SECONDS(CONFIG_UPDATE_STOP_FREQUENCY_SECONDS),
      K_SECONDS(CONFIG_UPDATE_STOP_FREQUENCY_SECONDS)
  );
  LOG_INF("update_stop_timer started");

  /* After LTE and after the display's own timer is armed. Non-blocking:
   * the shadow sync/telemetry cycle (including the immediate first sync
   * that applies e.g. a stop_id set from the dashboard before this boot)
   * runs on the pigeon client's OWN lower-priority thread, so an
   * unreachable PidgeIoT can neither delay this boot path nor starve the
   * watchdog this loop feeds. */
  pigeon_client_init();

  /* Both readers of the reset-cause register have now run -- this boot's
   * log line above, and pigeon_init() synchronously inside the call above
   * -- so it is safe to clear, and it has to be cleared by someone: this
   * SoC's RESETREAS is cumulative, a set bit surviving until written back.
   * Left alone it becomes the OR of every reset the board has ever taken,
   * which reports a fixed mask forever rather than this boot's cause, and
   * stops matching log_reset_reason()'s equality tests at all. */
  (void)hwinfo_clear_reset_cause();

  while (1) {
    if (k_sem_take(&rtc_sync_sem, K_NO_WAIT) == 0) {
      ret = set_rtc_time();
      if (ret) {
        LOG_ERR("Failed to set rtc.");
        goto reset;
      }
    } else {
      LOG_DBG("Failed to take rtc_sync_sem");
    }

    if (k_sem_take(&update_stop_sem, K_NO_WAIT) == 0) {
#ifdef CONFIG_LIGHT_SENSOR
      ret = update_stop();

#ifdef CONFIG_BOOTLOADER_MCUBOOT
      /* First proven-working departure fetch, empty stop included: NOW a
       * test-swapped image has earned permanent confirmation -- see
       * confirm_image_if_healthy()'s docs for the revert semantics. */
      if ((ret == 0) || (ret == UPDATE_STOP_NO_DEPARTURES)) {
        confirm_image_if_healthy();
      }
#endif  // CONFIG_BOOTLOADER_MCUBOOT

      if (ret == 0) {
        lux = get_lux();
        if (lux < 0) {
          goto reset;
        }
      } else if (ret == UPDATE_STOP_NO_DEPARTURES) {
        lux = 0xFF;
      } else if (pigeon_client_fota_active()) {
        /* A FOTA download saturates the LTE link enough to fail Swiftly
         * fetches; rebooting here would kill the download and burn a
         * persisted attempt (it burned all three in the first live OTA
         * test). Skip this cycle -- the display keeps its last state and
         * the reset policy resumes the moment the download ends. */
        LOG_WRN("update_stop failed during FOTA download; skipping reset policy this cycle");
      } else if (
          swiftly_consecutive_failures() < (unsigned int)CONFIG_UPDATE_FAILURES_BEFORE_RESET
      ) {
        /* Hold the last displayed times rather than resetting. See
         * UPDATE_FAILURES_BEFORE_RESET for why a lone failure is not
         * worth a reboot; the watchdog feed below still runs, so this
         * cannot mask a wedged device. */
        LOG_WRN(
            "update_stop failed (%u consecutive); holding display, not resetting yet",
            swiftly_consecutive_failures()
        );
      } else {
        LOG_ERR("update_stop failed %u times consecutively", swiftly_consecutive_failures());
        goto reset;
      }

      ret = pwm_leds_set((uint32_t)lux);
      if (ret) {
        goto reset;
      }
#else
      ret = update_stop();
      if (ret && (ret != UPDATE_STOP_NO_DEPARTURES)) {
        if (pigeon_client_fota_active()) {
          /* See the CONFIG_LIGHT_SENSOR branch's comment. */
          LOG_WRN("update_stop failed during FOTA download; skipping reset policy this cycle");
        } else if (
            swiftly_consecutive_failures() < (unsigned int)CONFIG_UPDATE_FAILURES_BEFORE_RESET
        ) {
          /* See the CONFIG_LIGHT_SENSOR branch's comment. */
          LOG_WRN(
              "update_stop failed (%u consecutive); holding display, not resetting yet",
              swiftly_consecutive_failures()
          );
        } else {
          LOG_ERR("update_stop failed %u times consecutively", swiftly_consecutive_failures());
          goto reset;
        }
      }

#ifdef CONFIG_BOOTLOADER_MCUBOOT
      confirm_image_if_healthy();
#endif  // CONFIG_BOOTLOADER_MCUBOOT
#endif  // CONFIG_LIGHT_SENSOR

      ret = wdt_feed(wdt, wdt_channel_id);
      if (ret) {
        LOG_ERR("Failed to feed watchdog. Err: %d", ret);
        goto reset;
      }
    } else {
      LOG_DBG("Failed to take update_stop_sem");
    }

    /* k_sleep, NOT k_cpu_idle: k_cpu_idle() never yields -- this prio-0
     * thread stays runnable through it, so every lower-priority thread
     * (the pigeon client thread; Zephyr's own deferred-log thread) would
     * starve forever. Sleeping blocks this thread between passes, letting
     * them run, while any of them is still preempted the instant this
     * loop's next pass is due. 100ms granularity is noise against the
     * 30s update cadence and the 60s watchdog window. */
    k_sleep(K_MSEC(100));
  }

reset:
  lte_disconnect();

#ifdef CONFIG_DEBUG
  LOG_WRN("Reached end of main; waiting for manual reset.");
  while (1) {
    ret = wdt_feed(wdt, wdt_channel_id);
    if (ret) {
      LOG_ERR("Failed to feed watchdog. Err: %d", ret);
    }
    k_msleep(29000);
  }
#else
  k_msleep(3000);
  LOG_ERR("Reached end of main; rebooting.");
  /* The ARM implementation sys_reboot ignores the parameter */
  sys_reboot(SYS_REBOOT_WARM);
#endif
}
#endif  // CONFIG_LED_DISPLAY_TEST
