/**
 * @file captive_portal.c
 * @brief Standalone captive portal implementation for ESP-IDF.
 * @author Ahmed Al-Tameemi, Nordes Sp. z o. o.
 *
 * Provides three complementary captive portal mechanisms:
 *   1. DHCP Option 114 (RFC 8910) - advertises the captive portal URI in
 *      DHCP offers/acks for clients that support standards-based discovery.
 *   2. HTTP redirect handlers - registers the well-known OS captive-portal
 *      probe URIs on the application's HTTP server and redirects every probe
 *      to the device's local web interface.
 *   3. DNS server - listens on UDP port 53 and answers all A-record queries
 *      with the soft-AP IP address, ensuring every DNS lookup performed by a
 *      connected client resolves to the device.
 *
 * Design notes:
 *   - Module-static storage holds the effective configuration so that the
 *     HTTP handler (which receives only httpd_req_t*) can access it without
 *     heap allocation.
 *   - The redirect URL is resolved at every request to always reflect the
 *     current AP IP address, which may change after runtime netif reconfiguration.
 *   - The static URI table is shared across all registrations; ESP-IDF stores
 *     only the pointer internally, so the table must have static storage lifetime.
 *   - The DNS server task clears s_dns_task_handle before self-deleting so that
 *     the signal_stop path can confirm clean task exit without a blocking wait.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"

#include "lwip/sockets.h"

#include "esp_event.h"
#include "esp_wifi.h"

#include "captive_portal.h"

static const char *TAG = "captive_portal";

/* --------------------------------------------------------------------------
 * Module-static configuration
 * Populated by captive_portal_apply_config() on every registration call.
 * Read by the HTTP handler and DNS task.
 * -------------------------------------------------------------------------- */
static struct {
    char     redirect_url[128]; /*!< Non-empty: fixed URL. Empty: auto-detect. */
    char     netif_key[32];     /*!< esp_netif interface key for IP resolution. */
    uint16_t redirect_port;     /*!< Web server port; 80 is omitted from URLs.  */
} s_cfg = {
    .redirect_url = "",
    .netif_key    = CONFIG_CAPTIVE_PORTAL_NETIF_KEY,
    .redirect_port = (uint16_t)(CONFIG_CAPTIVE_PORTAL_REDIRECT_PORT),
};

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Resolve the redirect target URL into @p buf.
 *
 * Resolution order:
 *   1. s_cfg.redirect_url if non-empty (fixed URL, used verbatim).
 *   2. IP obtained from the esp_netif identified by s_cfg.netif_key.
 *   3. CONFIG_CAPTIVE_PORTAL_FALLBACK_IP (compile-time fallback).
 */
static void build_redirect_url(char *buf, size_t buf_size)
{
    /* Fixed URL takes priority - copy and return immediately. */
    if (s_cfg.redirect_url[0] != '\0') {
        strlcpy(buf, s_cfg.redirect_url, buf_size);
        return;
    }

    /* Attempt to read the AP interface IP address. */
    const char *ip_str = CONFIG_CAPTIVE_PORTAL_FALLBACK_IP;
    char        ip_buf[16] = {0};

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(s_cfg.netif_key);
    if (netif != NULL) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_buf, sizeof(ip_buf), IPSTR, IP2STR(&ip_info.ip));
            ip_str = ip_buf;
        }
    }

    /* Build URL; omit port number for the standard HTTP port 80. */
    if (s_cfg.redirect_port == 80U) {
        snprintf(buf, buf_size, "http://%s/", ip_str);
    } else {
        snprintf(buf, buf_size, "http://%s:%u/", ip_str, (unsigned)s_cfg.redirect_port);
    }
}

/* --------------------------------------------------------------------------
 * HTTP request handler
 * -------------------------------------------------------------------------- */

/**
 * @brief HTTP GET handler for all registered captive-portal probe endpoints.
 *
 * Issues an HTTP 302 redirect with:
 *   - Location header pointing to the device's web interface.
 *   - A minimal HTML body for clients that do not follow redirects automatically.
 *   - Cache-Control: no-store to prevent caching of the redirect response.
 *   - Connection: close to inform the client the connection will not be reused.
 *   - An explicit server-side socket close via httpd_sess_trigger_close() to
 *     free the httpd socket slot immediately after sending the response.
 *
 * The explicit close is necessary because ESP-IDF httpd keeps a socket open
 * after the handler returns regardless of the Connection: close response header
 * (HTTP/1.1 keep-alive is the server default).  Since the built-in DNS server
 * redirects ALL hostnames to the AP IP, a burst of simultaneous connections
 * arrives from background OS/app traffic; without immediate socket release,
 * the httpd socket table fills up (errno 23 / ENFILE) and new connections are
 * refused until existing ones time out.
 */
static esp_err_t captive_portal_http_handler(httpd_req_t *req)
{
    char url[128];
    build_redirect_url(url, sizeof(url));

    /* Compact HTML with three redirect mechanisms for maximum client compatibility:
     *   1. <meta http-equiv="refresh">  - handled by most basic browsers.
     *   2. JavaScript window.location   - handled by modern browsers.
     *   3. A visible hyperlink          - fallback for users whose browsers block both. */
    /* Buffer must hold static template (~215 bytes) + url (up to 128 bytes) × 3. */
    char body[640];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html><html><head>"
             "<meta http-equiv=\"refresh\" content=\"0;url=%s\">"
             "<title>Redirecting</title></head><body>"
             "<p>Redirecting to <a href=\"%s\">device</a>...</p>"
             "<script>window.location.replace('%s');</script>"
             "</body></html>",
             url, url, url);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_sendstr(req, body);

    /* Force the server to close this socket as soon as the response is sent.
     * httpd_sess_trigger_close() queues a close on the socket fd; the actual
     * close happens after this handler returns, ensuring the response is fully
     * transmitted before the connection is torn down. */
    httpd_sess_trigger_close(req->handle, httpd_req_to_sockfd(req));

    ESP_LOGD(TAG, "Redirected '%s' -> '%s'", req->uri, url);
    return ESP_OK;
}

/* --------------------------------------------------------------------------
 * URI table
 *
 * Static lifetime required: ESP-IDF stores only the pointer to httpd_uri_t.
 * user_ctx is intentionally NULL - the handler reads config from s_cfg.
 * -------------------------------------------------------------------------- */
static const httpd_uri_t s_portal_uris[] = {
    /* Apple CNA (Captive Network Assistant) */
    { .uri = "/hotspot-detect.html",       .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
    { .uri = "/library/test/success.html", .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Android / Google connectivity check.
     * Both variants are used: /generate_204 (standard) and /generate204
     * (no underscore, sent by Chrome on some Android versions). */
    { .uri = "/generate_204",              .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
    { .uri = "/generate204",               .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
    { .uri = "/gen_204",                   .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Microsoft Windows / Xbox connectivity check */
    { .uri = "/connecttest.txt",           .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
    { .uri = "/ncsi.txt",                  .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Windows Web Proxy Auto-Discovery */
    { .uri = "/wpad.dat",                  .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Mozilla Firefox connectivity check */
    { .uri = "/success.txt",               .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Generic redirect endpoint */
    { .uri = "/redirect",                  .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },

    /* Chrome / Chromium browser network time (wildcard URI -
     * requires httpd_config_t.uri_match_fn = httpd_uri_match_wildcard) */
    { .uri = "/browsernetworktime/*",      .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
};

#define NUM_PORTAL_URIS  (sizeof(s_portal_uris) / sizeof(s_portal_uris[0]))

/* --------------------------------------------------------------------------
 * Wildcard catch-all URI table
 *
 * Registered by captive_portal_register_catchall() AFTER all application
 * handlers.  Catches every URI not matched by a previously registered handler.
 *
 * IMPORTANT: httpd evaluates URI handlers in registration order; the first
 * match wins.  Registering these before application handlers would intercept
 * all application traffic.  captive_portal_register_catchall() must therefore
 * be called last, after httpd_register_uri_handler() for all app routes.
 *
 * Note: requires httpd_config_t.uri_match_fn = httpd_uri_match_wildcard
 * (same requirement as /browsernetworktime/\* above; if the user followed
 * the Quick Start this is already set).
 *
 * Only GET and HEAD are registered.  These account for virtually all
 * background OS / browser traffic that generates unmatched connections.
 * POST and other methods to unknown URIs are rare from OS captive-portal
 * detection; they do not contribute measurably to socket exhaustion.
 * -------------------------------------------------------------------------- */
static const httpd_uri_t s_catchall_uris[] = {
    { .uri = "/*", .method = HTTP_GET,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
    { .uri = "/*", .method = HTTP_HEAD,
      .handler = captive_portal_http_handler, .user_ctx = NULL },
};

#define NUM_CATCHALL_URIS  (sizeof(s_catchall_uris) / sizeof(s_catchall_uris[0]))

/* --------------------------------------------------------------------------
 * DNS server (captive portal redirect for raw DNS queries)
 *
 * Handles all incoming DNS A-record queries by responding with the IP address
 * of the soft-AP interface.  Combined with the HTTP redirect handlers above,
 * this ensures that every OS captive-portal detection path - both HTTP-based
 * probes and DNS lookups - is intercepted by the device.
 * -------------------------------------------------------------------------- */

/** @brief Standard DNS port number. */
#define DNS_PORT 53

/** @brief Maximum DNS message size accepted and sent by this implementation. */
#define DNS_MAX_LEN 512

/**
 * @brief DNS message header layout (RFC 1035 §4.1.1).
 *
 * All fields are in network byte order; use ntohs()/htons() before access.
 */
typedef struct
{
    uint16_t id;         /*!< Identifier assigned by the querying program.      */
    uint16_t flags;      /*!< QR, opcode, AA, TC, RD, RA, Z, RCODE fields.      */
    uint16_t questions;  /*!< Number of entries in the question section.         */
    uint16_t answers;    /*!< Number of resource records in the answer section.  */
    uint16_t authority;  /*!< Number of name server resource records.            */
    uint16_t additional; /*!< Number of additional resource records.             */
} dns_header_t;

static int              s_dns_socket      = -1;
static TaskHandle_t     s_dns_task_handle = NULL;
static volatile bool    s_dns_running     = false;

/** Mutex serialising captive_portal_dns_start / _stop to prevent race
 *  conditions on rapid AP on/off transitions. Created once at module init. */
static SemaphoreHandle_t s_dns_mutex = NULL;

/**
 * @brief One-time initialiser for s_dns_mutex.
 *
 * Safe to call multiple times; creates the mutex only on the first call.
 */
static void dns_mutex_init(void)
{
    if (s_dns_mutex == NULL) {
        s_dns_mutex = xSemaphoreCreateMutex();
        /* Allocation from internal RAM; NULL here means the heap is exhausted,
         * which is non-recoverable at this point in the boot sequence. */
        configASSERT(s_dns_mutex != NULL);
    }
}

/**
 * @brief FreeRTOS task: UDP DNS server that answers all queries with the AP IP.
 *
 * Listens on UDP port DNS_PORT.  For every incoming DNS query (QR bit = 0)
 * it constructs a minimal A-record response pointing to the soft-AP interface
 * address resolved from s_cfg.netif_key and sends it back to the client.
 *
 * The task exits when s_dns_running is cleared by captive_portal_dns_stop(),
 * either upon the 1-second recvfrom() timeout expiring or when the socket is
 * closed externally to unblock the call immediately.
 *
 * @param pvParameters Unused; present to satisfy the xTaskCreate signature.
 */
static void dns_task(void *pvParameters)
{
    (void)pvParameters;

    struct sockaddr_in server_addr;
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    char rx_buffer[DNS_MAX_LEN];

    /* Create UDP socket */
    s_dns_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_socket < 0)
    {
        ESP_LOGE(TAG, "Failed to create DNS socket");
        s_dns_running = false;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(DNS_PORT);

    if (bind(s_dns_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0)
    {
        ESP_LOGE(TAG, "Failed to bind DNS socket to port %d", DNS_PORT);
        close(s_dns_socket);
        s_dns_socket = -1;
        s_dns_running = false;
        s_dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS server started on port %d", DNS_PORT);

    /* 1-second receive timeout allows the loop to re-check s_dns_running
     * without blocking indefinitely when no DNS traffic arrives.
     * Set once here; the value does not change during the task lifetime. */
    struct timeval tv = {.tv_sec = 1, .tv_usec = 0};
    setsockopt(s_dns_socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (s_dns_running)
    {
        int len = recvfrom(s_dns_socket, rx_buffer, sizeof(rx_buffer), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);

        if (!s_dns_running)
        {
            break;
        }

        if (len < 0)
        {
            if (errno == EWOULDBLOCK || errno == ETIMEDOUT)
            {
                /* Normal timeout - re-check s_dns_running and continue. */
                continue;
            }
            /* Unexpected socket error (e.g. socket closed by dns_stop). */
            ESP_LOGI(TAG, "DNS socket error (errno: %d), stopping server", errno);
            break;
        }

        if (len > 0)
        {
            dns_header_t *header = (dns_header_t *)rx_buffer;

            /* Only respond to queries (QR bit = 0); silently drop responses. */
            if ((ntohs(header->flags) & 0x8000) == 0)
            {
                /* Resolve the current AP IP address at request time. */
                esp_netif_ip_info_t ip_info = {0};
                esp_netif_t *netif = esp_netif_get_handle_from_ifkey(s_cfg.netif_key);
                if (netif != NULL)
                {
                    esp_netif_get_ip_info(netif, &ip_info);
                }

                /* Build response: copy original query, patch header, append A record. */
                char tx_buffer[DNS_MAX_LEN];
                memcpy(tx_buffer, rx_buffer, len);

                dns_header_t *resp_header = (dns_header_t *)tx_buffer;
                resp_header->flags = htons(0x8180); /* QR=1, AA=1, RD=1, RA=1, RCODE=0 */
                resp_header->answers = htons(1);    /* One A-record answer              */

                /* Append A-record answer (RFC 1035 §3.2.1):
                 *   NAME     : 0xC00C - compressed pointer to question section (offset 12)
                 *   TYPE     : 0x0001 - A record (IPv4 address)
                 *   CLASS    : 0x0001 - IN (Internet)
                 *   TTL      : 0x0000012C - 300 seconds
                 *   RDLENGTH : 0x0004 - 4 bytes
                 *   RDATA    : 4-byte IPv4 address */
                uint8_t *ans = (uint8_t *)(tx_buffer + len);
                *ans++ = 0xC0;
                *ans++ = 0x0C; /* NAME (compressed) */
                *ans++ = 0x00;
                *ans++ = 0x01; /* TYPE A            */
                *ans++ = 0x00;
                *ans++ = 0x01; /* CLASS IN          */
                *ans++ = 0x00;
                *ans++ = 0x00;
                *ans++ = 0x01;
                *ans++ = 0x2C; /* TTL 300 s  */
                *ans++ = 0x00;
                *ans++ = 0x04;                    /* RDLENGTH 4        */
                memcpy(ans, &ip_info.ip.addr, 4); /* RDATA: IP address */
                ans += 4;

                int response_len = (int)(ans - (uint8_t *)tx_buffer);
                sendto(s_dns_socket, tx_buffer, response_len, 0,
                       (struct sockaddr *)&client_addr, client_addr_len);

                ESP_LOGI(TAG, "DNS query answered with IP: " IPSTR, IP2STR(&ip_info.ip));
            }
        }
    }

    /* Snapshot and invalidate the socket fd before closing to prevent a
     * concurrent captive_portal_dns_stop() from closing the same fd again. */
    int sock = s_dns_socket;
    s_dns_socket = -1;
    if (sock >= 0)
    {
        close(sock);
    }

    s_dns_running     = false;
    s_dns_task_handle = NULL; /* Signal captive_portal_dns_stop() that the task has exited. */
    ESP_LOGI(TAG, "DNS server stopped");
    vTaskDelete(NULL);
}

/* --------------------------------------------------------------------------
 * DNS server API
 * -------------------------------------------------------------------------- */

static esp_err_t captive_portal_dns_start(void)
{
    dns_mutex_init();
    xSemaphoreTake(s_dns_mutex, portMAX_DELAY);

    if (s_dns_task_handle != NULL || s_dns_running)
    {
        ESP_LOGW(TAG, "DNS server start skipped - already running "
                      "(handle: %p, running: %d)",
                 s_dns_task_handle, (int)s_dns_running);
        xSemaphoreGive(s_dns_mutex);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Starting DNS server");
    s_dns_running = true; /* Set flag before creating the task to prevent a race. */

    BaseType_t ret = xTaskCreate(dns_task, "captive_dns", 4096, NULL, 5, &s_dns_task_handle);
    if (ret != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create DNS server task");
        s_dns_running = false;
        xSemaphoreGive(s_dns_mutex);
        return ESP_FAIL;
    }

    xSemaphoreGive(s_dns_mutex);
    return ESP_OK;
}

/**
 * @brief Signal the DNS server task to stop without blocking.
 *
 * Safe to call from any context including the Wi-Fi event loop task.
 * Clears @c s_dns_running and closes the socket to unblock @c recvfrom()
 * immediately.  The task will self-delete shortly afterwards.
 */
static void captive_portal_dns_signal_stop(void)
{
    if (!s_dns_running)
    {
        return;
    }

    ESP_LOGI(TAG, "Stopping DNS server");
    s_dns_running = false;

    /* Snapshot and invalidate before closing to prevent a double-close if the
     * dns_task socket-cleanup path runs concurrently. */
    int sock = s_dns_socket;
    s_dns_socket = -1;
    if (sock >= 0)
    {
        close(sock); /* Unblocks recvfrom() immediately. */
    }
}

/* --------------------------------------------------------------------------
 * Wi-Fi AP lifecycle event handler
 *
 * Registered once (by the first registration function called) via
 * captive_portal_enable_dns().  Starts and stops the DNS server in
 * lock-step with the Wi-Fi soft-AP so the caller never needs to manage
 * the DNS lifecycle explicitly.
 * -------------------------------------------------------------------------- */

/** Guard ensuring the Wi-Fi event handler is registered at most once. */
static bool s_event_handler_registered = false;

/**
 * @brief Internal Wi-Fi event handler for automatic DNS server lifecycle.
 *
 * Starts the DNS server when the soft-AP comes up and stops it when the
 * soft-AP goes down.  Registered once by captive_portal_register().
 *
 * @param arg        Unused; present to satisfy esp_event_handler_t.
 * @param event_base Must be WIFI_EVENT.
 * @param event_id   WIFI_EVENT_AP_START or WIFI_EVENT_AP_STOP.
 * @param event_data Unused.
 */
static void captive_portal_wifi_event_handler(void *arg,
                                              esp_event_base_t event_base,
                                              int32_t event_id,
                                              void *event_data)
{
    (void)arg;
    (void)event_base;
    (void)event_data;

    if (event_id == WIFI_EVENT_AP_START) {
        captive_portal_dns_start();
    } else if (event_id == WIFI_EVENT_AP_STOP) {
        /* captive_portal_dns_stop() blocks waiting for the task to exit, which
         * is illegal from within an esp_event handler.  Signal-only stop is
         * used here; the task will self-delete asynchronously. */
        captive_portal_dns_signal_stop();
    }
}

/* --------------------------------------------------------------------------
 * Public API helpers
 * -------------------------------------------------------------------------- */

/**
 * @brief Apply caller-supplied configuration into the module-static s_cfg store.
 *
 * Extracts effective values from @p config (falling back to Kconfig defaults
 * for any NULL / zero field) and copies them into @c s_cfg so that the HTTP
 * handler task and DNS task can read them without receiving additional
 * parameters.
 *
 * Must be called inside every public registration function before the handler
 * registration loop and before captive_portal_enable_dns().
 *
 * @param config  Runtime configuration, or NULL to use all Kconfig defaults.
 */
static void captive_portal_apply_config(const captive_portal_config_t *config)
{
    const char *eff_url  = NULL;
    const char *eff_key  = CONFIG_CAPTIVE_PORTAL_NETIF_KEY;
    uint16_t    eff_port = (uint16_t)(CONFIG_CAPTIVE_PORTAL_REDIRECT_PORT);

    if (config != NULL) {
        if (config->redirect_url  != NULL) { eff_url  = config->redirect_url;  }
        if (config->netif_key     != NULL) { eff_key  = config->netif_key;     }
        if (config->redirect_port != 0U)   { eff_port = config->redirect_port; }
    }

    if (eff_url != NULL) {
        strlcpy(s_cfg.redirect_url, eff_url, sizeof(s_cfg.redirect_url));
    } else {
        s_cfg.redirect_url[0] = '\0';
    }
    strlcpy(s_cfg.netif_key, eff_key, sizeof(s_cfg.netif_key));
    s_cfg.redirect_port = eff_port;
}

/**
 * @brief Configure DHCP Option 114 (RFC 8910) with the portal URI.
 *
 * The URI is built from the same effective redirect configuration used by the
 * HTTP redirect handler.  This keeps DHCP and HTTP announcements consistent.
 *
 * DHCP option updates are applied by briefly restarting the DHCP server on the
 * selected netif.  Stop/start errors are logged and tolerated where possible,
 * while option-set failure is returned to the caller.
 *
 * @return ESP_OK on success, ESP_ERR_INVALID_STATE if netif is unavailable,
 *         or an esp_err_t from esp_netif_dhcps_option on failure.
 */
static esp_err_t captive_portal_set_dhcp_option_114(void)
{
#if CONFIG_CAPTIVE_PORTAL_ENABLE_DHCP_OPTION_114
    char captive_uri[128];
    build_redirect_url(captive_uri, sizeof(captive_uri));

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey(s_cfg.netif_key);
    if (netif == NULL) {
        ESP_LOGW(TAG, "DHCP Option 114 skipped: netif '%s' not found", s_cfg.netif_key);
        return ESP_ERR_INVALID_STATE;
    }

    /* Restart DHCP server to ensure the option is reloaded with fresh value. */
    esp_err_t stop_err = esp_netif_dhcps_stop(netif);
    if (stop_err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP server stop before Option 114 returned: %s",
                 esp_err_to_name(stop_err));
    }

    esp_err_t opt_err = esp_netif_dhcps_option(netif,
                                               ESP_NETIF_OP_SET,
                                               ESP_NETIF_CAPTIVEPORTAL_URI,
                                               captive_uri,
                                               strlen(captive_uri));
    if (opt_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set DHCP Option 114 URI '%s': %s",
                 captive_uri, esp_err_to_name(opt_err));
        /* Best effort: restore DHCP server state even when option update fails. */
        esp_netif_dhcps_start(netif);
        return opt_err;
    }

    esp_err_t start_err = esp_netif_dhcps_start(netif);
    if (start_err != ESP_OK) {
        ESP_LOGW(TAG, "DHCP server start after Option 114 returned: %s",
                 esp_err_to_name(start_err));
    }

    ESP_LOGI(TAG, "DHCP Option 114 set to '%s'", captive_uri);
    return ESP_OK;
#else
    return ESP_OK;
#endif
}

/**
 * @brief Register Wi-Fi AP event handlers and start the DNS server if the AP
 *        interface is already up.
 *
 * Idempotent: the Wi-Fi event handlers are registered at most once regardless
 * of how many times this function is called.  captive_portal_dns_start() also
 * guards against double-start internally.
 *
 * Must be called after captive_portal_apply_config() so that s_cfg.netif_key
 * is valid for the esp_netif_is_netif_up() check.
 */
static void captive_portal_enable_dns(void)
{
    if (!s_event_handler_registered) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                   captive_portal_wifi_event_handler, NULL);
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                   captive_portal_wifi_event_handler, NULL);
        s_event_handler_registered = true;
    }

    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey(s_cfg.netif_key);
    if (ap_netif != NULL && esp_netif_is_netif_up(ap_netif)) {
        captive_portal_dns_start();
    }

    ESP_LOGI(TAG, "Captive portal ready (netif: '%s', port: %u)",
             s_cfg.netif_key, (unsigned)s_cfg.redirect_port);
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */

/**
 * @brief Register all specific captive-portal probe URIs and start DNS (advanced API).
 *
 * Registers the complete set of 11 OS captive-portal detection endpoints
 * individually, applies configuration, applies DHCP Option 114 (if enabled),
 * and enables automatic DNS lifecycle
 * management (Wi-Fi AP start/stop events + immediate start if the AP is
 * already up).
 *
 * Must be called AFTER all application URI handlers have been registered,
 * consistent with the other two public registration functions.
 *
 * @param server  A valid @c httpd_handle_t returned by @c httpd_start().
 *                Must not be NULL.
 * @param config  Pointer to a configuration struct, or NULL to use the
 *                Kconfig compile-time defaults for all fields.
 *
 * @return
 *   - @c ESP_OK                      – All probe handlers registered and DNS active.
 *   - @c ESP_ERR_INVALID_ARG         – @p server is NULL.
 *   - @c ESP_ERR_HTTPD_HANDLERS_FULL – Handler table is full; increase
 *                                      @c httpd_config_t.max_uri_handlers.
 *   - @c ESP_FAIL                    – An unexpected registration error occurred.
 */
esp_err_t captive_portal_register_uris(httpd_handle_t server,
                                       const captive_portal_config_t *config)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "captive_portal_register_uris: server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    captive_portal_apply_config(config);
    captive_portal_set_dhcp_option_114();

    for (size_t i = 0; i < NUM_PORTAL_URIS; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_portal_uris[i]);
        if (err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "URI '%s' already registered, skipping",
                     s_portal_uris[i].uri);
        } else if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register URI '%s': %s",
                     s_portal_uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "Captive portal: registered %zu specific probe URI handlers",
             NUM_PORTAL_URIS);
    captive_portal_enable_dns();
    return ESP_OK;
}

/**
 * @brief Register the wildcard catch-all URI handlers (HTTP only, no DNS).
 *
 * Internal helper used by captive_portal_register_catchall() (public) and as
 * the fallback path inside captive_portal_register_http().  Registers @c /\*
 * handlers for GET and HEAD without touching configuration or DNS.
 *
 * Registration is atomic: if the HEAD handler cannot be registered after the
 * GET handler was already registered, the GET handler is removed and the error
 * is returned, leaving the server in its original state.
 *
 * @param server  A valid, non-NULL @c httpd_handle_t (caller must verify).
 *
 * @return @c ESP_OK on success, @c ESP_ERR_HTTPD_HANDLERS_FULL if the URI
 *         table is full, or another esp_err_t on unexpected failure.
 */
static esp_err_t captive_portal_register_catchall_http(httpd_handle_t server)
{
    bool registered[NUM_CATCHALL_URIS] = {0};

    for (size_t i = 0; i < NUM_CATCHALL_URIS; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_catchall_uris[i]);
        if (err == ESP_OK) {
            registered[i] = true;
        } else if (err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            ESP_LOGW(TAG, "Catch-all handler for method %d already registered, skipping",
                     (int)s_catchall_uris[i].method);
        } else {
            ESP_LOGE(TAG, "Failed to register catch-all handler (method %d): %s",
                     (int)s_catchall_uris[i].method, esp_err_to_name(err));
            /* Atomic rollback: remove any handler registered in this call. */
            for (size_t j = 0; j < i; j++) {
                if (registered[j]) {
                    httpd_unregister_uri_handler(server,
                                                 s_catchall_uris[j].uri,
                                                 s_catchall_uris[j].method);
                }
            }
            return err;
        }
    }

    ESP_LOGI(TAG, "Captive portal: catch-all mode — unmatched URIs will redirect to portal");
    return ESP_OK;
}

/**
 * @brief Register wildcard catch-all URI handlers and start DNS (advanced API).
 *
 * Registers @c /\* handlers for GET and HEAD, applies configuration, applies
 * DHCP Option 114 (if enabled), and enables automatic DNS lifecycle management (Wi-Fi AP start/stop events +
 * immediate start if the AP is already up).
 *
 * Requests not matched by any previously registered handler are redirected to
 * the portal and the socket is closed immediately, preventing ENFILE
 * exhaustion from background OS and browser traffic.
 *
 * Must be called AFTER all application URI handlers so that the wildcard does
 * not shadow application routes.
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
                                           const captive_portal_config_t *config)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "captive_portal_register_catchall: server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    captive_portal_apply_config(config);
    captive_portal_set_dhcp_option_114();

    esp_err_t ret = captive_portal_register_catchall_http(server);
    if (ret != ESP_OK) {
        return ret;
    }

    captive_portal_enable_dns();
    return ESP_OK;
}

/**
 * @brief Auto-select and apply the HTTP handler registration strategy.
 *
 * Attempts to register all specific probe URI handlers one by one.  If the
 * httpd handler table is exhausted before all can be registered, any
 * partially registered handlers are rolled back atomically, and the function
 * falls back to the wildcard catch-all strategy instead.
 *
 * The available slot count acts as an implicit signal of the deployment role:
 *   - Servers that have raised @c max_uri_handlers and retain enough free
 *     slots receive specific URI matching.  This is preferable for devices
 *     that also serve application content, because a @c /\* wildcard could
 *     otherwise intercept requests intended for application routes.
 *   - Servers operating on the default 8-slot configuration (or with limited
 *     remaining slots) automatically receive the wildcard catch-all.  This is
 *     equally effective for pure captive-portal deployments and uses only two
 *     handler slots.
 *
 * @param server  Validated httpd handle (caller guarantees non-NULL).
 *
 * @return @c ESP_OK on success, or the error returned by the failing
 *         @c httpd_register_uri_handler call.
 */
static esp_err_t captive_portal_register_http(httpd_handle_t server)
{
    bool registered[NUM_PORTAL_URIS] = {0};
    bool slots_full = false;

    for (int i = 0; i < (int)NUM_PORTAL_URIS; i++) {
        esp_err_t err = httpd_register_uri_handler(server, &s_portal_uris[i]);
        if (err == ESP_OK) {
            registered[i] = true;
        } else if (err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
            /* Already registered by a previous call or the user — skip. */
            ESP_LOGD(TAG, "URI '%s' already registered, skipping", s_portal_uris[i].uri);
        } else if (err == ESP_ERR_HTTPD_HANDLERS_FULL) {
            slots_full = true;
            break;
        } else {
            ESP_LOGE(TAG, "URI '%s' registration error: %s",
                     s_portal_uris[i].uri, esp_err_to_name(err));
            /* Atomic rollback of everything we registered in this call. */
            for (int j = 0; j < (int)NUM_PORTAL_URIS; j++) {
                if (registered[j]) {
                    httpd_unregister_uri_handler(server,
                                                 s_portal_uris[j].uri,
                                                 s_portal_uris[j].method);
                }
            }
            return err;
        }
    }

    if (!slots_full) {
        int n = 0;
        for (int i = 0; i < (int)NUM_PORTAL_URIS; i++) {
            if (registered[i]) { n++; }
        }
        ESP_LOGI(TAG, "Captive portal: specific URI mode (%d probe handlers registered)", n);
        return ESP_OK;
    }

    /* --- Handler table full: roll back partial registrations and fall back -- */
    {
        int n_partial = 0;
        for (int i = 0; i < (int)NUM_PORTAL_URIS; i++) {
            if (registered[i]) { n_partial++; }
        }
        ESP_LOGW(TAG,
                 "Captive portal: URI table full after %d/%zu handlers — "
                 "rolling back and switching to wildcard catch-all mode",
                 n_partial, NUM_PORTAL_URIS);
    }

    for (int j = 0; j < (int)NUM_PORTAL_URIS; j++) {
        if (registered[j]) {
            httpd_unregister_uri_handler(server,
                                         s_portal_uris[j].uri,
                                         s_portal_uris[j].method);
        }
    }

    return captive_portal_register_catchall_http(server);
}

/**
 * @brief Register the captive portal using automatic HTTP strategy selection.
 *
 * Applies runtime configuration, applies DHCP Option 114 (if enabled),
 * selects either specific URI mode or catch-all mode based on available
 * handler slots, then enables automatic DNS lifecycle management tied to
 * Wi-Fi AP start/stop events.
 *
 * Expected call order is after all application URI handlers are registered.
 * This ensures that, when fallback catch-all mode is selected, the wildcard
 * route remains last and does not shadow application routes.
 *
 * @param server  Active HTTP server handle. Must not be NULL.
 * @param config  Optional runtime configuration. NULL uses Kconfig defaults.
 *
 * @return
 *   - @c ESP_OK                      on success.
 *   - @c ESP_ERR_INVALID_ARG         if @p server is NULL.
 *   - @c ESP_ERR_HTTPD_HANDLERS_FULL if no URI slots are available even for
 *                                     catch-all fallback mode.
 *   - Other @c esp_err_t values propagated from httpd URI registration APIs.
 */
esp_err_t captive_portal_register(httpd_handle_t server,
                                  const captive_portal_config_t *config)
{
    if (server == NULL) {
        ESP_LOGE(TAG, "captive_portal_register: server handle is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    captive_portal_apply_config(config);
    captive_portal_set_dhcp_option_114();

    esp_err_t result = captive_portal_register_http(server);
    if (result != ESP_OK) {
        return result;
    }

    captive_portal_enable_dns();
    return ESP_OK;
}
