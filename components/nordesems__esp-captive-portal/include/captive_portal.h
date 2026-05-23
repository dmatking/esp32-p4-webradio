/**
 * @file captive_portal.h
 * @brief Standalone captive portal component for ESP-IDF.
 * @author Ahmed Al-Tameemi, Nordes Sp. z o. o.
 *
 * Provides two complementary captive portal mechanisms:
 *
 *   0. DHCP Option 114 (RFC 8910) - advertises the captive portal URI in
 *      DHCP offers/acks so supporting clients can discover the portal without
 *      probe heuristics.
 *
 *   1. HTTP redirect handlers – registers the standard OS captive-portal
 *      detection endpoints on a running ESP-IDF HTTP server.  A 302 redirect
 *      to the device's local web interface is returned for every probe request,
 *      causing iOS, macOS, Android, Windows, and Firefox to automatically
 *      display the built-in configuration page when a user connects to the device's Wi-Fi AP.
 *
 *   2. DNS server – listens on UDP port 53 and answers every DNS A-record
 *      query with the soft-AP IP address, ensuring connected clients resolve
 *      all hostnames to the device and triggering the OS captive-portal flow.
 *
 * Detected OS probe endpoints (HTTP):
 *   - iOS / macOS : /hotspot-detect.html, /library/test/success.html
 *   - Android     : /generate_204, /generate204, /gen_204
 *   - Windows     : /connecttest.txt, /ncsi.txt, /wpad.dat
 *   - Firefox     : /success.txt
 *   - Generic     : /redirect, /browsernetworktime/[wildcard]
 *
 * Minimal usage (Kconfig defaults):
 * @code
 *   httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
 *   cfg.uri_match_fn    = httpd_uri_match_wildcard;  // required for /browsernetworktime/
 *   httpd_handle_t server = NULL;
 *   httpd_start(&server, &cfg);
 *
 *   // Register your application handlers first (if any)
 *   httpd_register_uri_handler(server, &my_page);
 *
 *   // Register the captive portal last — it auto-selects the best strategy
 *   captive_portal_register(server, NULL);
 * @endcode
 *
 * Custom redirect URL:
 * @code
 *   captive_portal_config_t portal_cfg = CAPTIVE_PORTAL_CONFIG_DEFAULT();
 *   portal_cfg.redirect_url = "http://192.168.1.1/";
 *   captive_portal_register(server, &portal_cfg);
 * @endcode
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Captive portal runtime configuration.
 *
 * All fields are optional. NULL / zero values fall back to the corresponding
 * Kconfig compile-time defaults (see sdkconfig).
 */
typedef struct {
    /**
     * @brief Fixed redirect target URL, e.g. @c "http://192.168.4.1/".
     *
     * If non-NULL this URL is used verbatim for every redirect response.
     * If NULL the component resolves the IP at request time by querying
     * the network interface identified by @p netif_key, falling back to
     * CONFIG_CAPTIVE_PORTAL_FALLBACK_IP on failure.
     */
    const char *redirect_url;

    /**
     * @brief esp_netif interface key used for dynamic IP detection.
     *
     * Ignored when @p redirect_url is set.
     * NULL defaults to @c CONFIG_CAPTIVE_PORTAL_NETIF_KEY.
     *
     * Common values:
     *   - @c "WIFI_AP_DEF"  – Wi-Fi soft-AP   (default)
     *   - @c "WIFI_STA_DEF" – Wi-Fi station
     *   - @c "ETH_DEF"      – Ethernet
     */
    const char *netif_key;

    /**
     * @brief TCP port of the target web server.
     *
     * 0 defaults to @c CONFIG_CAPTIVE_PORTAL_REDIRECT_PORT (normally 80).
     * Port 80 is omitted from the generated redirect URL (standard HTTP).
     */
    uint16_t redirect_port;
} captive_portal_config_t;

/**
 * @brief Zero-initializer that applies the Kconfig defaults for all fields.
 *
 * @code
 *   captive_portal_config_t cfg = CAPTIVE_PORTAL_CONFIG_DEFAULT();
 *   cfg.redirect_url = "http://10.0.0.1/";   // override one field, keep rest
 *   captive_portal_register(server, &cfg);
 * @endcode
 */
#define CAPTIVE_PORTAL_CONFIG_DEFAULT() {                               \
    .redirect_url  = NULL,                                              \
    .netif_key     = CONFIG_CAPTIVE_PORTAL_NETIF_KEY,                  \
    .redirect_port = (uint16_t)(CONFIG_CAPTIVE_PORTAL_REDIRECT_PORT),  \
}

/**
 * @brief Register the captive portal on a running HTTP server (recommended API).
 *
 * This is the primary single-call API. It applies a reliable auto-selection
 * strategy for HTTP URI registration and then starts DNS lifecycle management.
 * Before HTTP URI registration, this API also applies DHCP Option 114 (if
 * enabled in Kconfig) so DHCP-aware clients can use that signal first.
 *
 * Registration strategy:
 *   1. Try registering all 11 specific probe URIs.
 *   2. If the HTTP server reports @c ESP_ERR_HTTPD_HANDLERS_FULL before all
 *      11 fit, roll back any partial registrations from this call and fall
 *      back to wildcard catch-all mode (@c /\* for GET and HEAD).
 *
 * This lets the component adapt to both common deployment styles:
 *   - Default constrained servers (@c HTTPD_DEFAULT_CONFIG() with
 *     @c max_uri_handlers = 8): catch-all mode is selected automatically.
 *   - Servers with expanded URI capacity: specific URI mode is selected,
 *     preserving normal behavior for broader application traffic.
 *
 * After HTTP strategy selection this function also:
 *   - Registers Wi-Fi AP start/stop event handlers (once) to manage DNS.
 *   - Starts DNS immediately if the AP netif is already up.
 *
 * @note  Must be called after all application URI handlers are registered.
 *        This allows wildcard fallback mode (if selected) to remain last.
 *
 * @note  Requires wildcard URI matching:
 *        @c httpd_config_t.uri_match_fn = httpd_uri_match_wildcard
 *
 * @param server  A valid @c httpd_handle_t returned by @c httpd_start().
 *                Must not be NULL.
 * @param config  Pointer to a configuration struct, or NULL to use the
 *                Kconfig compile-time defaults for all fields.
 *
 * @return
 *   - @c ESP_OK                      – Registration strategy applied and DNS
 *                                      lifecycle management active.
 *   - @c ESP_ERR_INVALID_ARG         – @p server is NULL.
 *   - @c ESP_ERR_HTTPD_HANDLERS_FULL – Even fallback catch-all could not be
 *                                      registered due to URI table exhaustion.
 *   - @c ESP_FAIL                    – Unexpected registration error.
 */
esp_err_t captive_portal_register(httpd_handle_t server,
                                  const captive_portal_config_t *config);

/**
 * @brief Register all known captive-portal probe URIs and start DNS (advanced API).
 *
 * Registers the complete specific URI probe set (11 endpoints), applies
 * configuration, applies DHCP Option 114 (if enabled), and enables automatic
 * DNS lifecycle management tied to
 * Wi-Fi AP start/stop events (same as @c captive_portal_register(), but
 * with an explicit specific-URI strategy instead of auto-selection).
 *
 * Use this API when your server has enough free URI slots and you want
 * explicit specific-URI mode instead of auto-selection.  For automatic
 * strategy selection, prefer @c captive_portal_register().
 *
 * @note  Must be called after all application URI handlers.
 *
 * @note  Requires wildcard URI matching for @c /browsernetworktime/\*:
 *        @c httpd_config_t.uri_match_fn = httpd_uri_match_wildcard
 *
 * @param server  A valid @c httpd_handle_t returned by @c httpd_start().
 *                Must not be NULL.
 * @param config  Pointer to a configuration struct, or NULL to use the
 *                Kconfig compile-time defaults for all fields.
 *
 * @return
 *   - @c ESP_OK                      – All probe handlers registered and DNS active.
 *   - @c ESP_ERR_INVALID_ARG         – @p server is NULL.
 *   - @c ESP_ERR_HTTPD_HANDLERS_FULL – URI table is full.
 *   - @c ESP_FAIL                    – Unexpected registration error.
 */
esp_err_t captive_portal_register_uris(httpd_handle_t server,
                                       const captive_portal_config_t *config);

/**
 * @brief Register wildcard catch-all URI handlers and start DNS (advanced API).
 *
 * Registers @c /\* handlers for GET and HEAD, applies configuration, and
 * applies DHCP Option 114 (if enabled), and enables automatic DNS lifecycle
 * management tied to Wi-Fi AP start/stop
 * events (same as @c captive_portal_register(), but with an explicit
 * catch-all strategy instead of auto-selection).
 *
 * Requests not matched by any previously registered handler are redirected
 * to the portal and the socket is closed immediately, preventing ENFILE
 * exhaustion from background OS and browser traffic.
 *
 * Must be called AFTER all application URI handlers so that the wildcard
 * does not shadow application routes.
 *
 * @note  Requires @c httpd_config_t.uri_match_fn = httpd_uri_match_wildcard.
 *
 * @param server  A valid @c httpd_handle_t returned by @c httpd_start().
 *                Must not be NULL.
 * @param config  Pointer to a configuration struct, or NULL to use the
 *                Kconfig compile-time defaults for all fields.
 *
 * @return
 *   - @c ESP_OK                      – Catch-all handlers registered and DNS active.
 *   - @c ESP_ERR_INVALID_ARG         – @p server is NULL.
 *   - @c ESP_ERR_HTTPD_HANDLERS_FULL – URI table is full.
 *   - @c ESP_FAIL                    – Unexpected registration error.
 */
esp_err_t captive_portal_register_catchall(httpd_handle_t server,
                                           const captive_portal_config_t *config);

#ifdef __cplusplus
}
#endif
