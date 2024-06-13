/** @headerfile custom_http_client.h */
#include "custom_http_client.h"

#include <app_version.h>
#include <stdlib.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/storage/stream_flash.h>

#include "fota.h"
#include "lte_manager.h"

LOG_MODULE_REGISTER(custom_http_client, LOG_LEVEL_DBG);

/* Setup TLS options on a given socket */
int tls_setup(int fd, char *hostname, sec_tag_t sec_tag) {
  int err;
  int verify;

  /* Security tag that we have provisioned the certificate with */
  sec_tag_t tls_sec_tag[] = {sec_tag};

  /* Set up TLS peer verification */
  enum {
    NONE = 0,
    OPTIONAL = 1,
    REQUIRED = 2,
  };

  verify = REQUIRED;

  err = setsockopt(fd, SOL_TLS, TLS_PEER_VERIFY, &verify, sizeof(verify));
  if (err) {
    LOG_ERR("Failed to setup peer verification, %s", strerror(errno));
    return err;
  }

  err = setsockopt(
      fd, SOL_TLS, TLS_SEC_TAG_LIST, tls_sec_tag, sizeof(tls_sec_tag)
  );
  if (err) {
    LOG_ERR("Failed to setup TLS sec tag, %s", strerror(errno));
    return err;
  }

  err = setsockopt(fd, SOL_TLS, TLS_HOSTNAME, hostname, strlen(hostname));
  if (err) {
    LOG_ERR("Failed to setup TLS hostname, %s", strerror(errno));
    return err;
  }

  return 0;
}

static int parse_status(char *headers_buf) {
  char *ptr;
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
      LOG_INF("Informational response code: %d", code);
      return HTTP_INFO;
    case 200 ... 299:
      LOG_INF("Successful response code: %d", code);
      return HTTP_SUCCESS;
    case 300 ... 399:
      LOG_INF("Redirection response code: %d", code);
      return HTTP_REDIRECT;
    case 400 ... 499:
      LOG_INF("Client error response code: %d", code);
      return HTTP_CLIENT_ERROR;
    case 500 ... 599:
      LOG_INF("Server error response code: %d", code);
      return HTTP_SERVER_ERROR;
    default:
      LOG_ERR("HTTP status code missing or incorrect. Response code: %d", code);
      return HTTP_NULL;
  }
}

static int parse_headers(int *sock, char *headers_buf, int headers_buf_size) {
  int state = 0;
  int bytes;
  int status;
  size_t headers_size = 0;
  size_t headers_offset = 0;

  do {
    bytes = recv(*sock, &headers_buf[headers_offset], 1, 0);
    if (bytes < 0) {
      LOG_ERR("recv() headers failed, %s", strerror(errno));
      return bytes;
    }

    if ((state == 0 || state == 2) && headers_buf[headers_offset] == '\r') {
      state++;
    } else if (state == 1 && headers_buf[headers_offset] == '\n') {
      state++;
    } else if (state == 3) {
      headers_size = headers_offset;
      LOG_INF("Received Headers. Size: %d bytes", headers_offset);

      headers_buf[headers_offset + 1] = '\0';

      status = parse_status(headers_buf);
      switch (status) {
        case HTTP_REDIRECT:
          return 1;
        case HTTP_CLIENT_ERROR:
          return -1;
        case HTTP_SERVER_ERROR:
          return -1;
        case HTTP_NULL:
          return -1;
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
    int *sock, char *recv_body_buf, int recv_body_buf_size, long offset,
    char *headers_buf, int headers_buf_size, _Bool write_nvs
) {
  int rc = 0;
  int bytes;

  size_t headers_size = parse_headers(sock, headers_buf, headers_buf_size);
  if (headers_size < 1) {
    return -1;
  }

  do {
    if (write_nvs) {
      bytes = recv(*sock, recv_body_buf, recv_body_buf_size, 0);
      if (bytes < 0) {
        if (errno == EMSGSIZE) {
          LOG_WRN(
              "recv() returned EMSGSIZE. Modem's secure socket buffer limit "
              "reached.\nRetrying with new socket, Range: %ld-",
              offset
          );
          return offset;
        }
        LOG_ERR("recv() body failed, %s", strerror(errno));
        return bytes;
      }
      LOG_DBG("recv bytes: %d", bytes);
      LOG_DBG("Total: %ld", offset);
      rc = write_buffer_to_flash(recv_body_buf, bytes, false);
      if (rc < 0) {
        LOG_ERR("write_buffer_to_flash() failed, error: %s", strerror(errno));
        return bytes;
      }
    } else {
      bytes = recv(
          *sock, (recv_body_buf + offset), (recv_body_buf_size - offset), 0
      );
      if (bytes < 0) {
        LOG_ERR("recv() body failed, %s", strerror(errno));
        return bytes;
      }
    }
    offset += bytes;
  } while (bytes != 0);

  LOG_INF("Received Body. Size: %ld bytes", offset);
  LOG_INF("Total bytes received: %ld", offset + headers_size);

  if (write_nvs) {
    rc = write_buffer_to_flash(recv_body_buf, 0, true);
    if (rc < 0) {
      LOG_ERR("write_buffer_to_flash() failed, error: %s", strerror(errno));
      return bytes;
    }
    return rc;
  }

  /* Make sure recv_buf is NULL terminated (for safe use with strstr) */
  if (offset < recv_body_buf_size) {
    *(recv_body_buf + offset) = '\0';
  } else {
    *(recv_body_buf + recv_body_buf_size - 1) = '\0';
  }

  return 0;
}

static char *get_redirect_location(char *headers_buf, int headers_buf_size) {
  char *ptr;

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

  *strstr(ptr, "\r\n") = '\0';

  return ptr;
}

static int send_http_request(
    char *hostname, char *path, char *accept, sec_tag_t sec_tag,
    char *recv_body_buf, int recv_body_buf_size, char *headers_buf,
    int headers_buf_size, _Bool write_nvs
) {
  int bytes;
  int err;
  int headers_size;
  size_t offset;
  char *ptr;
  long rc = 0;
  int sock = -1;
  long range_start = 0;

  struct addrinfo *addr_inf;
  static struct addrinfo hints = {
      .ai_socktype = SOCK_STREAM, .ai_flags = AI_NUMERICSERV
  };

  LOG_DBG("hostname: %s", hostname);
  LOG_DBG("path: %s", path);
  LOG_DBG("accept: %s", accept);
  LOG_DBG("SEC_TAG: %d", sec_tag);

retry:
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
  // TODO: Change APP_VERSION_STRING to APP_VERSION_TWEAK_STRING when possible
  ptr = stpcpy(
      ptr,
      "User-Agent: EDB/" APP_VERSION_STRING " Stop-ID/" CONFIG_STOP_ID "\r\n"
  );
  if (range_start > 0) {
    ptr = stpcpy(ptr, "Range: ");
    ptr += sprintf(ptr, "%ld", range_start);
    ptr = stpcpy(ptr, "-\r\n");
  }
  ptr = stpcpy(ptr, "Accept: ");
  ptr = stpcpy(ptr, accept);
  ptr = stpcpy(ptr, "\r\nConnection: close\r\n\r\n");

  headers_size = (ptr - &headers_buf[0]);

  LOG_DBG("Send Headers (size: %d):\n%s", headers_size, &headers_buf[0]);

  if (sec_tag == NO_SEC_TAG) {
    err = getaddrinfo(hostname, "80", &hints, &addr_inf);
  } else {
    err = getaddrinfo(hostname, "443", &hints, &addr_inf);
  }
  if (err) {
    LOG_ERR("getaddrinfo() failed, %s", strerror(errno));
    return errno;
  }

  char peer_addr[INET6_ADDRSTRLEN];

  if (inet_ntop(
          addr_inf->ai_family,
          &((struct sockaddr_in *)(addr_inf->ai_addr))->sin_addr, peer_addr,
          INET6_ADDRSTRLEN
      ) == NULL) {
    LOG_ERR("inet_ntop() failed, %s", strerror(errno));
    return errno;
  }

  LOG_INF("Resolved %s (%s)", peer_addr, net_family2str(addr_inf->ai_family));

  if (sec_tag == NO_SEC_TAG) {
    sock = socket(addr_inf->ai_family, SOCK_STREAM, addr_inf->ai_protocol);
  } else if (IS_ENABLED(CONFIG_MBEDTLS)) {
    sock = socket(
        addr_inf->ai_family, SOCK_STREAM | SOCK_NATIVE_TLS, IPPROTO_TLS_1_2
    );
  } else {
    sock = socket(addr_inf->ai_family, SOCK_STREAM, IPPROTO_TLS_1_2);
  }

  if (sock == -1) {
    LOG_ERR("Failed to open socket!\n");
    goto clean_up;
  }

  if (sec_tag != NO_SEC_TAG) {
    err = tls_setup(sock, hostname, sec_tag);
    if (err) {
      goto clean_up;
    }
  }

  LOG_DBG(
      "Connecting to %s:%d", hostname,
      ntohs(((struct sockaddr_in *)(addr_inf->ai_addr))->sin_port)
  );

  err = connect(sock, addr_inf->ai_addr, addr_inf->ai_addrlen);
  if (err) {
    LOG_ERR("connect() failed, %s", strerror(errno));
    goto clean_up;
  }

  LOG_DBG(
      "addrinfo @%p: ai_family=%d, ai_socktype=%d, ai_protocol=%d, "
      "sa_family=%d, sin_port=%x\n",
      addr_inf, addr_inf->ai_family, addr_inf->ai_socktype,
      addr_inf->ai_protocol, addr_inf->ai_addr->sa_family,
      ((struct sockaddr_in *)addr_inf->ai_addr)->sin_port
  );

  offset = 0;
  do {
    bytes = send(sock, &headers_buf[offset], headers_size - offset, 0);
    if (bytes < 0) {
      LOG_ERR("send() failed, %s", strerror(errno));
      goto clean_up;
    }
    offset += bytes;
  } while (offset < headers_size);

  LOG_INF("Sent %d bytes", offset);

  rc = parse_response(
      &sock, recv_body_buf, recv_body_buf_size, range_start, headers_buf,
      headers_buf_size, write_nvs
  );
  if (rc < 0) {
    LOG_ERR("EOF or error in response headers.");
  }

clean_up:
  LOG_INF("Closing socket %d", sock);
  err = close(sock);
  if (err) {
    LOG_ERR("close() failed, %s", strerror(errno));
  }

  (void)freeaddrinfo(addr_inf);

  LOG_DBG("Response Headers:\n%s", &headers_buf[0]);

  if (rc == 1) {
    goto redirect;
  } else if (rc > 1) {
    range_start = rc;
    goto retry;
  }

  return 0;

redirect:
  ptr = get_redirect_location(headers_buf, headers_buf_size);
  char redirect_hostname_buf[255];
  LOG_INF("Redirect location: %s", ptr);

  /* Assume the host is the same */
  if (*ptr == '/') {
    path = strcpy(redirect_hostname_buf, ptr);
    LOG_DBG("path ptr: %s", path);
    /* Assume we're dealing with a url */
  } else if (*ptr == 'h') {
    if (strncmp(ptr, "https", 5) == 0) {
      if (sec_tag == NO_SEC_TAG) {
        LOG_ERR(
            "Redirect requires TLS, but a TLS sec_tag was assigned. Assign "
            "correct sec_tag in the code "
            "Aborting."
        );
        return 1;
      }
      ptr += 8;
    } else {
      ptr += 7;
      sec_tag = NO_SEC_TAG;
    }
    hostname = ptr;
    ptr = strstr(ptr, "/");
    *ptr++ = '\0';

    path = stpcpy(redirect_hostname_buf, hostname);
    *++path = '/';

    (void)strcpy((path + 1), ptr);
    hostname = redirect_hostname_buf;
  } else {
    LOG_ERR("Bad redirect location");
    return 1;
  }

  goto retry;
}

int http_request_stop_json(
    char *stop_body_buf, int stop_body_buf_size, char *headers_buf,
    int headers_buf_size
) {
  int err;

  /** Make the size 255 incase we get a redirect with a longer hostname */
  static char hostname[255] = CONFIG_STOP_REQUEST_HOSTNAME;

  /** Make the size 255 incase we get a redirect with a longer path */
  static char path[255] = CONFIG_STOP_REQUEST_PATH;

  if (k_sem_take(&lte_connected_sem, K_SECONDS(30)) != 0) {
    LOG_ERR("Failed to take lte_connected_sem");
    err = 1;
  } else {
#if CONFIG_STOP_REQUEST_AVAIL
    err = send_http_request(
        hostname, path, "application/json", NO_SEC_TAG, stop_body_buf,
        stop_body_buf_size, headers_buf, headers_buf_size, false
    );
    k_sem_give(&lte_connected_sem);
#else
    err = send_http_request(
        hostname, path, "application/json", JES_SEC_TAG, stop_body_buf,
        stop_body_buf_size, false
    );
    k_sem_give(&lte_connected_sem);
#endif
  }

  return err;
}

int http_get_firmware(
    char *write_buf, int write_buf_size, char *headers_buf, int headers_buf_size
) {
  int err;

  if (k_sem_take(&lte_connected_sem, K_FOREVER) != 0) {
    LOG_ERR("Failed to take lte_connected_sem");
    err = 1;
  } else {
    err = send_http_request(
        CONFIG_OTA_HOSTNAME, CONFIG_OTA_PATH, "application/octet-stream",
        JES_SEC_TAG, write_buf, write_buf_size, headers_buf, headers_buf_size,
        true
    );

    k_sem_give(&lte_connected_sem);
  }

  return err;
}
