/** @headerfile lte_manager.h */
#include "lte_manager.h"

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Both credential stores are in play since the pigeon integration:
 * Swiftly's CA cert always goes into Zephyr's NATIVE store (its socket is
 * pinned to SOCK_NATIVE_TLS/mbedTLS -- see custom_http_client.c), while
 * pigeon's CA cert goes into the MODEM's store when CONFIG_MODEM_KEY_MGMT
 * is available (the pigeon library's sockets are plain offloaded
 * IPPROTO_TLS_1_2, i.e. TLS inside the modem). */
#include <zephyr/net/tls_credentials.h>

#ifdef CONFIG_MODEM_KEY_MGMT
#include <modem/modem_key_mgmt.h>
#endif

#include "watchdog_app.h"

LOG_MODULE_REGISTER(lte_manager);

static const char swiftly_cert[] = {
#include "AmazonRootCA3.cer.hex"
    // Null terminate certificate if running Mbed TLS
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};

#if defined(CONFIG_PIGEON)
/* GTS Root R4 -- api.pidgeiot.com sits behind the Google Trust Services
 * chain, distinct from Swiftly's Amazon root, so it gets its own
 * array/sec_tag. */
static const char pigeon_cert[] = {
#include "r4.crt.hex"
    IF_ENABLED(CONFIG_TLS_CREDENTIALS, (0x00))
};
#endif  // CONFIG_PIGEON

#ifdef CONFIG_MODEM_KEY_MGMT
// The total size of the included certificates must be less than 4KB
BUILD_ASSERT(
    (
        sizeof(swiftly_cert)
#if defined(CONFIG_PIGEON)
        + sizeof(pigeon_cert)
#endif  // CONFIG_PIGEON
    ) < KB(4),
    "Certificates too large"
);
#endif

K_SEM_DEFINE(lte_connected_sem, 1, 1);

#ifdef CONFIG_NRF_MODEM_LIB_ON_FAULT_APPLICATION_SPECIFIC
void nrf_modem_fault_handler(struct nrf_modem_fault_info* fault_info) {
  LOG_ERR("Reason: %d", fault_info->reason);
  LOG_ERR("Program Counter: %d", fault_info->program_counter);
  LOG_ERR("%s", nrf_modem_lib_fault_strerror(errno));
}
#endif

/* Provision a CA certificate into Zephyr's native TLS credential store
 * (consumed by SOCK_NATIVE_TLS/mbedTLS sockets -- the Swiftly path). */
static int provision_native_cert(sec_tag_t sec_tag, const char cert[], size_t cert_len) {
  int err = tls_credential_add(sec_tag, TLS_CREDENTIAL_CA_CERTIFICATE, cert, cert_len);
  if (err == -EEXIST) {
    LOG_INF("CA certificate already exists, sec tag: %d", sec_tag);
  } else if (err < 0) {
    LOG_ERR("Failed to register CA certificate: %d", err);
    return err;
  }
  return 0;
}

#if defined(CONFIG_PIGEON) && defined(CONFIG_MODEM_KEY_MGMT)
/* Provision a CA certificate into the MODEM's credential store (consumed
 * by plain offloaded IPPROTO_TLS_1_2 sockets -- the pigeon path). Must run
 * before lte_lc_connect(): the modem rejects %CMNG writes once registered.
 * Skips the write when the stored cert already matches, so a normal boot
 * costs one compare instead of a delete+write of modem NVM. */
static int provision_modem_cert(nrf_sec_tag_t sec_tag, const char cert[], size_t cert_len) {
  int err;
  bool exists;

  err = modem_key_mgmt_exists(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, &exists);
  if (err) {
    LOG_ERR("Failed to check for certificates err %d", err);
    return err;
  }

  if (exists) {
    err = modem_key_mgmt_cmp(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, cert, cert_len);
    if (err == 0) {
      return 0;
    }

    err = modem_key_mgmt_delete(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN);
    if (err) {
      LOG_ERR("Failed to delete existing certificate, err %d", err);
    }
  }

  err = modem_key_mgmt_write(sec_tag, MODEM_KEY_MGMT_CRED_TYPE_CA_CHAIN, cert, cert_len);
  if (err) {
    LOG_ERR("Failed to provision certificate, err %d", err);
    return err;
  }

  return 0;
}
#endif  // CONFIG_PIGEON && CONFIG_MODEM_KEY_MGMT

int lte_connect(void) {
  int err;
  k_sem_take(&lte_connected_sem, K_FOREVER);

#if CONFIG_NRF_MODEM_LIB
  err = nrf_modem_lib_init();
  if (err) {
    LOG_ERR("Failed to initialize modem library!");
    return err;
  }
#endif

  /* Provision certificates before connecting to the network */
  err = provision_native_cert(SWIFTLY_SEC_TAG, swiftly_cert, sizeof(swiftly_cert));
  if (err) {
    LOG_ERR("Failed to provision TLS certificate. TLS_SEC_TAG: %d", SWIFTLY_SEC_TAG);
    return err;
  }

#if defined(CONFIG_PIGEON)
#ifdef CONFIG_MODEM_KEY_MGMT
  err = provision_modem_cert(PIGEON_SEC_TAG, pigeon_cert, sizeof(pigeon_cert));
#else
  /* Without modem_key_mgmt the pigeon library's offloaded TLS sockets have
   * no cert to find in the modem store -- this fallback only works if the
   * pigeon module is built with CONFIG_PIGEON_HTTPS_NATIVE_TLS so its
   * sockets read the native store instead. */
  err = provision_native_cert(PIGEON_SEC_TAG, pigeon_cert, sizeof(pigeon_cert));
#endif  // CONFIG_MODEM_KEY_MGMT
  if (err) {
    LOG_ERR("Failed to provision TLS certificate. TLS_SEC_TAG: %d", PIGEON_SEC_TAG);
    return err;
  }
#endif  // CONFIG_PIGEON

  LOG_INF("Initializing LTE interface");
  err = wdt_feed(wdt, wdt_channel_id);
  if (err) {
    LOG_ERR("Failed to feed watchdog. Err: %d", err);
    return 1;
  }

  LOG_INF("Connecting to the network");
  err = lte_lc_connect();
  if (err < -1) {
    LOG_ERR("LTE failed to connect. Err: %d", err);
    return err;
  }

  k_sem_give(&lte_connected_sem);

  return 0;
}

int lte_disconnect(void) {
  int err;

  /* A small delay for the TCP connection teardown */
  k_sleep(K_SECONDS(1));

  err = lte_lc_power_off();
  if (err) {
    LOG_ERR("Failed to shutdown nrf modem. Err: %d", err);
    return err;
  }

  return err;
}
