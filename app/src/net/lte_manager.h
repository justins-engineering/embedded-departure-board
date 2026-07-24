/** @file connection_manager.h
 *  @brief Macros and function defines for the zephyr connection manager.
 */
#ifndef LTE_MANAGER_H
#define LTE_MANAGER_H

extern struct k_sem lte_connected_sem;
/* PIGEON_SEC_TAG must match CONFIG_PIGEON_HTTPS_SEC_TAG (see app/prj.conf) --
 * the pigeon library only opens sockets against that tag, it doesn't
 * provision the CA cert itself (see lte_manager.c's provision_cert() call
 * for it). */
enum tls_sec_tags { NO_SEC_TAG, SWIFTLY_SEC_TAG, PIGEON_SEC_TAG };

/** @fn int lte_connect(void)
 *  @brief Makes an HTTP GET request and returns a char pointer to the HTTP
 * response body buffer.
 */
int lte_connect(void);

/** @fn int lte_disconnect(void)
 *  @brief Makes an HTTP GET request and returns a char pointer to the HTTP
 * response body buffer.
 */
int lte_disconnect(void);
#endif
