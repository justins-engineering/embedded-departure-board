/** @headerfile custom_http_client.h */
#include "custom_http_client.h"

#include <stdlib.h>
#include <string.h>
#include <zephyr/app_version.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/storage/stream_flash.h>

#include "net/lte_manager.h"
#include "net/pigeon_client.h"
#include "runtime_config.h"
#include "stop_id.h"
#include "watchdog_app.h"

LOG_MODULE_REGISTER(custom_http_client);

#if defined(CONFIG_LTE_RAI_REQ)
/* Floor on the fetch cadence below which end-of-data RAI is NOT asserted.
 * US LTE-M carriers hold RRC connected for a ~10-20s inactivity timer
 * after the last packet. Above this floor the network releases between
 * fetches anyway, so RAI only trims the dead connected-mode tail -- a
 * pure win (same one service request per fetch, seconds less airtime).
 * BELOW the carrier's timer the connection would have stayed warm across
 * fetches for free, and forcing a release would ADD an RRC setup per
 * fetch -- at the shadow-clamped 5s minimum that is ~17k extra setups a
 * day, the exact signaling load AS-RAI exists to avoid. 15s splits the
 * observed timer range; if the bench's %CONEVAL/+CSCON measurement pins
 * this carrier's timer, tune this to sit just above it. */
#define RAI_MIN_FETCH_INTERVAL_S 15
#endif  // CONFIG_LTE_RAI_REQ

static const char swiftly_api_key[] = {
#include "../keys/private/swiftly-api.key"
};

enum response_code {
  HTTP_NULL,
  HTTP_INFO,
  HTTP_SUCCESS,
  HTTP_REDIRECT,
  HTTP_CLIENT_ERROR,
  HTTP_SERVER_ERROR
};

/* Setup TLS options on a given socket */
int tls_setup(int fd, char* hostname, sec_tag_t sec_tag) {
  int err;

  /* Security tag that we have provisioned the certificate with */
  sec_tag_t tls_sec_tag[] = {sec_tag};

  socklen_t verify = TLS_PEER_VERIFY_REQUIRED;

  err = zsock_setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
  if (err) {
    LOG_ERR("Failed to setup peer verification, Err: %s (%d)", strerror(errno), errno);
    return err;
  }

  err = zsock_setsockopt(fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag, sizeof(tls_sec_tag));
  if (err) {
    LOG_ERR("Failed to setup TLS sec tag, Err: %s (%d)", strerror(errno), errno);
    return err;
  }

  err = zsock_setsockopt(fd, SOL_TLS, TLS_HOSTNAME, hostname, strlen(hostname));
  if (err) {
    LOG_ERR("Failed to setup TLS hostname, Err: %s (%d)", strerror(errno), errno);
    return err;
  }

  /* Unconditional, previously guarded by CONFIG_MBEDTLS_SSL_CACHE_C -- the
   * wrong symbol: that is mbedTLS's SERVER-side session cache, never set in
   * this build, so this block compiled out and every fetch paid a full
   * handshake (cert chain + ECDHE) despite the code reading as if resumption
   * were on. The CLIENT-side cache this option actually drives is Zephyr's
   * own, always compiled into sockets_tls.c (client_cache[], gated only by
   * NET_SOCKETS_TLS_MAX_CLIENT_SESSION_COUNT and per-socket by this exact
   * sockopt -- see tls_session_save()/ztls_socket_data_check()). With it on,
   * repeat connections to Swiftly resume via session ID/ticket
   * (CONFIG_MBEDTLS_SSL_SESSION_TICKETS is already =y in the board conf):
   * no certificate chain on the wire, one less round trip, less mbedTLS
   * heap/CPU churn -- meaningful at this sign's 30s fetch cadence. */
  socklen_t session_cache = TLS_SESSION_CACHE_ENABLED;

  err = zsock_setsockopt(fd, SOL_TLS, TLS_SESSION_CACHE, &session_cache, sizeof(session_cache));
  if (err) {
    LOG_ERR("Unable to set TLS session cache, Err: %s (%d)", strerror(errno), errno);
    return err;
  }

  return EXIT_SUCCESS;
}

static int parse_status(char* headers_buf) {
  char* ptr;
  int code;

  ptr = strstr(headers_buf, "HTTP");
  if (ptr == NULL) {
    LOG_ERR("Missing HTTP status line");
    return HTTP_NULL;
  }

  ptr = strstr(headers_buf, " ");
  if (ptr == NULL) {
    LOG_ERR("Improperly formatted HTTP status line");
    return HTTP_NULL;
  }

  code = atoi(ptr);

  switch (code) {
    case 100 ... 199:
      LOG_WRN("Informational response code: %d", code);
      return HTTP_INFO;
    case 200 ... 299:
      LOG_INF("Successful response code: %d", code);
      return HTTP_SUCCESS;
    case 300 ... 399:
      LOG_WRN("Redirection response code: %d", code);
      return HTTP_REDIRECT;
    case 400 ... 499:
      LOG_ERR("Client error response code: %d", code);
      return HTTP_CLIENT_ERROR;
    case 500 ... 599:
      LOG_ERR("Server error response code: %d", code);
      return HTTP_SERVER_ERROR;
    default:
      LOG_ERR("HTTP status code missing or incorrect. Response code: %d", code);
      return HTTP_NULL;
  }
}

static int parse_headers(int* sock, char* headers_buf, int headers_buf_size) {
  int state = 0;
  int bytes;
  int status;
  size_t headers_size = 0;
  size_t headers_offset = 0;

  do {
    /* Bounds check (added with the RAM diet, 2026-08-08): this loop had
     * NONE -- a response with a header block larger than headers_buf
     * walked straight past the buffer into adjacent statics. -1 leaves
     * room for the NUL written after the terminator match below. */
    if (headers_offset >= (size_t)headers_buf_size - 1) {
      LOG_ERR("Response headers exceed %d-byte buffer", headers_buf_size);
      return -5;
    }

    bytes = zsock_recv(*sock, &headers_buf[headers_offset], 1, 0);
    if (bytes < 0) {
      LOG_ERR("recv() headers failed. Err %d: %s", errno, strerror(errno));
      return bytes;
    }

    if ((state == 0 || state == 2) && headers_buf[headers_offset] == '\r') {
      state++;
    } else if (state == 1 && headers_buf[headers_offset] == '\n') {
      state++;
    } else if (state == 3) {
      headers_size = headers_offset;
      LOG_DBG("Received Headers. Size: %d bytes", headers_offset);

      headers_buf[headers_offset + 1] = '\0';

      status = parse_status(headers_buf);
      switch (status) {
        case HTTP_REDIRECT:
          return -2;
        case HTTP_CLIENT_ERROR:
          return -3;
        case HTTP_SERVER_ERROR:
          return -4;
        case HTTP_NULL:
          return -5;
        default:
          break;
      }
      break;
    } else {
      state = 0;
    }
    headers_offset += bytes;
  } while (bytes != 0);

  return headers_size;
}

static long parse_response(
    int* sock, char* recv_body_buf, int recv_body_buf_size, long offset, char* headers_buf,
    int headers_buf_size, _Bool write_nvs
) {
  int bytes;

  int headers_size = parse_headers(sock, headers_buf, headers_buf_size);

  if (headers_size == 0) {
    // This should never occur because the headers_buf will still containd the send headers
    LOG_ERR("Empty response HEADERs");
    return -1;
  } else if (headers_size < 0) {
    return headers_size;
  }

  do {
    bytes = zsock_recv(*sock, (recv_body_buf + offset), (recv_body_buf_size - offset), 0);
    if (bytes < 0) {
      LOG_ERR("recv() body failed, %s", strerror(errno));
      return bytes;
    }
    offset += bytes;
  } while (bytes != 0);

  LOG_DBG("Received Body. Size: %ld bytes", offset);
  LOG_INF("Total bytes received: %ld", offset + headers_size);

  /* Make sure recv_buf is NULL terminated (for safe use with strstr) */
  if (offset < recv_body_buf_size) {
    *(recv_body_buf + offset) = '\0';
  } else {
    *(recv_body_buf + recv_body_buf_size - 1) = '\0';
  }

  return EXIT_SUCCESS;
}

static char* get_redirect_location(char* headers_buf, int headers_buf_size) {
  char* ptr;

  ptr = strstr(headers_buf, "Location:");
  if (ptr == NULL) {
    ptr = strstr(headers_buf, "location:");
    if (ptr == NULL) {
      LOG_ERR("Location header missing");
      return NULL;
    }
  }

  ptr += 9;

  while (*ptr == ' ') {
    ptr++;
  }

  char* end = strstr(ptr, "\r\n");
  if (end == NULL) {
    LOG_ERR("Location header not terminated");
    return NULL;
  }
  *end = '\0';

  return ptr;
}

static int send_http_request(
    char* hostname, char* path, char* accept, sec_tag_t sec_tag, char* recv_body_buf,
    int recv_body_buf_size, char* headers_buf, int headers_buf_size, _Bool write_nvs
) {
  int bytes;
  int err;
  int headers_size;
  size_t offset;
  char* ptr;
  long rc = 0;
  int sock = -1;
  long range_start = 0;
  // Keep track of retry attempts so we don't get in a loop
  int retry_client_error = 0;

  struct zsock_addrinfo* addr_inf;
  static struct zsock_addrinfo hints = {.ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV};

  /* Snapshotted once per request: the pigeon client thread may rewrite the
   * shared buffer at any moment (stop_id.h), and a header must not tear. */
  char stop_id[STOP_ID_MAX_LEN];

  stop_id_get(stop_id, sizeof(stop_id));

retry:
  err = wdt_feed(wdt, wdt_channel_id);
  if (err) {
    LOG_ERR("Failed to feed watchdog. Err: %d", err);
    return EXIT_FAILURE;
  }

  ptr = stpcpy(&headers_buf[0], "GET ");
  ptr = stpcpy(ptr, path);
  ptr = stpcpy(ptr, " HTTP/1.1\r\n");
  ptr = stpcpy(ptr, "Host: ");
  ptr = stpcpy(ptr, hostname);
  if (sec_tag == NO_SEC_TAG) {
    ptr = stpcpy(ptr, ":80\r\n");
  } else {
    ptr = stpcpy(ptr, ":443\r\n");
  }
  ptr = stpcpy(ptr, "User-Agent: EDB/" APP_VERSION_TWEAK_STRING " Stop-ID/");
  ptr = stpcpy(ptr, stop_id);
  ptr = stpcpy(ptr, "\r\n");
  if (range_start > 0) {
    ptr = stpcpy(ptr, "Range: ");
    ptr += sprintf(ptr, "%ld", range_start);
    ptr = stpcpy(ptr, "-\r\n");
  }
  ptr = stpcpy(ptr, "Accept: ");
  ptr = stpcpy(ptr, accept);
  ptr = stpcpy(ptr, "\r\n");

  if (sec_tag == SWIFTLY_SEC_TAG) {
    ptr = stpcpy(ptr, "Authorization: ");
    ptr = stpcpy(ptr, swiftly_api_key);
    ptr = stpcpy(ptr, "\r\n");
  }

  ptr = stpcpy(ptr, "Connection: close\r\n\r\n");

  headers_size = (ptr - &headers_buf[0]);

  LOG_DBG("Send Headers (size: %d):\n%s", headers_size, &headers_buf[0]);

  if (sec_tag == NO_SEC_TAG) {
    err = zsock_getaddrinfo(hostname, "80", &hints, &addr_inf);
  } else {
    err = zsock_getaddrinfo(hostname, "443", &hints, &addr_inf);
  }
  if (err) {
    LOG_ERR("getaddrinfo() failed, %s", strerror(errno));
    return errno;
  }

  char peer_addr[INET6_ADDRSTRLEN];

  if (zsock_inet_ntop(
          addr_inf->ai_family, &((struct sockaddr_in*)(addr_inf->ai_addr))->sin_addr, peer_addr,
          INET6_ADDRSTRLEN
      ) == NULL) {
    LOG_ERR("inet_ntop() failed, %s", strerror(errno));
    return errno;
  }

  LOG_DBG("Resolved %s (%s)", peer_addr, net_family2str(addr_inf->ai_family));

  if (sec_tag == NO_SEC_TAG) {
    sock = zsock_socket(addr_inf->ai_family, SOCK_STREAM, addr_inf->ai_protocol);
  } else {
    /* Always native TLS (mbedTLS over an offloaded TCP socket), no longer
     * keyed on CONFIG_MODEM_KEY_MGMT: the modem's own TLS can't take
     * Swiftly's large records (git history: "offloaded secure sockets has
     * a restriction of 2k recv limit" -- the reason this path migrated to
     * native TLS), and this path's CA certs live in Zephyr's native
     * credential store either way (lte_manager.c). CONFIG_MODEM_KEY_MGMT
     * being on no longer implies modem TLS here -- since the pigeon
     * integration it exists to provision the PIGEON cert into the modem
     * store, whose small-record traffic is what modem TLS is fine for. */
    sock = zsock_socket(addr_inf->ai_family, SOCK_STREAM | SOCK_NATIVE_TLS, IPPROTO_TLS_1_2);
  }

  if (sock == -1) {
    LOG_ERR("Failed to open socket!");
    goto clean_up;
  }

  if (sec_tag != NO_SEC_TAG) {
    err = tls_setup(sock, hostname, sec_tag);
    if (err) {
      goto clean_up;
    }
  }

  LOG_DBG(
      "Connecting to %s:%d", hostname, ntohs(((struct sockaddr_in*)(addr_inf->ai_addr))->sin_port)
  );

  err = zsock_connect(sock, addr_inf->ai_addr, addr_inf->ai_addrlen);
  if (err) {
    LOG_ERR("connect() failed. Err %d: %s", errno, strerror(errno));
    rc = -3;
    goto clean_up;
  }

  LOG_DBG(
      "Socket %d addrinfo: ai_family=%d, ai_socktype=%d, ai_protocol=%d, "
      "sa_family=%d, sin_port=%x",
      sock, addr_inf->ai_family, addr_inf->ai_socktype, addr_inf->ai_protocol,
      addr_inf->ai_addr->sa_family, ((struct sockaddr_in*)addr_inf->ai_addr)->sin_port
  );

  offset = 0;
  do {
    bytes = zsock_send(sock, &headers_buf[offset], headers_size - offset, MSG_WAITACK);
    if (bytes < 0) {
      LOG_ERR("send() failed, %s", strerror(errno));
      goto clean_up;
    }
    offset += bytes;
  } while (offset < headers_size);

  LOG_INF("Sent %d bytes", offset);

  rc = parse_response(
      &sock, recv_body_buf, recv_body_buf_size, range_start, headers_buf, headers_buf_size,
      write_nvs
  );
  if (rc == -1) {
    LOG_ERR("EOF or error in response headers.");
    /* On this path headers_buf still holds the SENT request, credential
     * included -- mask the Authorization value in place before dumping,
     * so a console (or captured console log) never sees the API key. The
     * buffer is rebuilt from scratch at the top of every (re)try, so
     * mutating it here is safe. */
    char* auth_value = strstr(headers_buf, "Authorization: ");

    if (auth_value != NULL) {
      for (auth_value += sizeof("Authorization: ") - 1;
           (*auth_value != '\r') && (*auth_value != '\0'); auth_value++) {
        *auth_value = '*';
      }
    }
    printk("%s\n", headers_buf);
    rc = -3;
  }

#if defined(CONFIG_LTE_RAI_REQ)
  /* End-of-data release assistance, only when this cycle is truly done:
   * a complete response is in (rc == EXIT_SUCCESS here can't mean the
   * send-failure goto -- that jumps straight to clean_up below) and no
   * retry/redirect/range continuation will follow. RAI_LAST rather than
   * RAI_NO_DATA, deliberately: the socket still has exactly one uplink
   * left -- zsock_close()'s TLS close_notify record -- and RAI_LAST
   * ("idle after the next output operation completes") sequences the RRC
   * release AFTER it, where NO_DATA ("release now") would race our own
   * teardown bytes into a fresh RRC setup. On the plain-HTTP redirect
   * path there is no close_notify, the hint just never fires, and the
   * carrier's inactivity timer applies as before -- never worse than
   * today. Skipped while a FOTA download streams (its per-chunk
   * connections on the pigeon thread need the link held), and below
   * RAI_MIN_FETCH_INTERVAL_S (see its comment). */
  if ((rc == EXIT_SUCCESS) && !pigeon_client_fota_active() &&
      (pigeon_client_update_stop_interval_s() >= RAI_MIN_FETCH_INTERVAL_S)) {
    int rai_option = RAI_LAST;

    err = zsock_setsockopt(sock, SOL_SOCKET, SO_RAI, &rai_option, sizeof(rai_option));
    if (err) {
      LOG_WRN("Failed to set SO_RAI, Err: %s (%d)", strerror(errno), errno);
    }
  }
#endif  // CONFIG_LTE_RAI_REQ

clean_up:
  LOG_DBG("Closing socket %d", sock);
  err = zsock_close(sock);
  if (err) {
    LOG_ERR("close() failed, %s", strerror(errno));
  }

  (void)zsock_freeaddrinfo(addr_inf);

  LOG_DBG("Response Headers:\n%s", &headers_buf[0]);

  if (rc == -2) {
    // Redirect returned; follow redirect
    goto redirect;
  } else if (rc > 1) {
    // Partial transefer complete; reconnect with new range request
    range_start = rc;
    goto retry;
  } else if ((rc == -3) && (retry_client_error < runtime_config_http_retry_count())) {
    LOG_WRN("HTTP request GET %s%s failed, retrying...", hostname, path);
    retry_client_error++;
    goto retry;
  } else if (rc < 0) {
    LOG_ERR("HTTP request GET %s%s failed", hostname, path);
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;

redirect:
  ptr = get_redirect_location(headers_buf, headers_buf_size);
  char redirect_hostname_buf[255];
  LOG_INF("Redirect location: %s", ptr);

  if (ptr == NULL) {
    LOG_ERR("get_redirect_location returned NULL pointer");
    return EXIT_FAILURE;
  } else {
    /* Assume the host is the same */
    if (*ptr == '/') {
      path = strcpy(redirect_hostname_buf, ptr);
      LOG_DBG("path ptr: %s", path);
      /* Assume we're dealing with a url */
    } else if (*ptr == 'h') {
      if (strncmp(ptr, "https", 5) == 0) {
        if (sec_tag == NO_SEC_TAG) {
          LOG_ERR(
              "Redirect requires TLS, but no TLS sec_tag was assigned. Assign "
              "correct sec_tag in the code "
              "Aborting."
          );
          return EXIT_FAILURE;
        }
        ptr += 8;
      } else {
        ptr += 7;
        sec_tag = NO_SEC_TAG;
      }
      hostname = ptr;
      ptr = strstr(ptr, "/");

      if (ptr == NULL) {
        LOG_ERR("strstr returned NULL pointer");
        return EXIT_FAILURE;
      } else {
        *ptr++ = '\0';

        path = stpcpy(redirect_hostname_buf, hostname);
        *++path = '/';

        (void)strcpy((path + 1), ptr);
        hostname = redirect_hostname_buf;
      }
    } else {
      LOG_ERR("Bad redirect location");
      return EXIT_FAILURE;
    }
  }

  goto retry;
}

int http_request_stop_json(
    char* stop_body_buf, int stop_body_buf_size, char* headers_buf, int headers_buf_size
) {
  int err;

  /** Make the size 255 incase we get a redirect with a longer hostname */
  static char hostname[255] = CONFIG_SWIFTLY_API_HOSTNAME;

  /** Make the size 255 incase we get a redirect with a longer path */
  static char path[255];

  /* Rebuilt every call (not just once at static-init time) so a stop_id
   * shadow update (stop_id.h) takes effect on the very next poll -- a
   * redirect within send_http_request() only rebinds its own local path/
   * hostname pointers into its own stack buffer, never writes back into
   * this static array, so reinitializing it here on entry is safe. */
  char stop_id[STOP_ID_MAX_LEN];

  stop_id_get(stop_id, sizeof(stop_id));
  snprintk(
      path, sizeof(path), "%s?stop=%s&number=%s", CONFIG_SWIFTLY_API_PATH, stop_id,
      CONFIG_SWIFTLY_API_NUMBER_OF_PREDICTIONS
  );

  if (k_sem_take(&lte_connected_sem, K_FOREVER) != 0) {
    LOG_ERR("Failed to take lte_connected_sem");
    err = 1;
  } else {
    err = send_http_request(
        hostname, path, "application/json", SWIFTLY_SEC_TAG, stop_body_buf, stop_body_buf_size,
        headers_buf, headers_buf_size, false
    );

    k_sem_give(&lte_connected_sem);
  }

  return err;
}
