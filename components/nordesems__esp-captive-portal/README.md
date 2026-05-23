# esp-captive-portal

A standalone, zero-dependency ESP-IDF component that implements a **captive portal** for Wi-Fi AP mode. After a single function call, any device that connects to your ESP32 soft-AP (iOS, macOS, Android, Windows, Firefox, Chrome) will automatically be redirected to your device's built-in web interface.

>**NOTE** Some devices DO NOT support captive portal for security reasons - this is the only limitation!

---

## Features

- **Single function call** — call once after registering your app handlers; DNS server lifecycle is fully automatic
- **DHCP Option 114 (RFC 8910)** — advertises captive portal URI in DHCP (enabled by default)
- **Built-in DNS server** — intercepts all DNS queries and redirects them to the AP IP, triggering captive portal detection on every OS
- **Automatic IP detection** — reads the AP interface IP at request time; survives runtime IP changes
- **All major OS probes handled** — iOS/macOS, Android, Windows/Xbox, Firefox, Chrome/Chromium (11 endpoints)
- **HTTP 302 redirect** — with meta-refresh and JS fallback for older clients
- **Fully configurable** via Kconfig or runtime `captive_portal_config_t`
- **Idempotent registration** — safe to call after an HTTP server restart

---

## Installation

### Option A — Espressif Component Registry (command-line/GUI)

Install directly from the Espressif Component Registry using the command-line.

- Using the `esp` CLI:
```
esp component install nordesems/esp-captive-portal
```

- Or using `idf.py` component manager:
```
idf.py add-dependency "nordesems/esp-captive-portal^1.3.0"
idf.py update-dependencies
```

**Or use the included ESP-IDF component registery GUI, search for `esp-captive-portal` and install.**

Espressif's tooling will fetch and install the component into your project.

### Option B — ESP-IDF Component Manager (idf_component.yml)

Add to your `main/idf_component.yml`:

```yaml
dependencies:
  esp-captive-portal: ">=1.3.0"
```

Then run:

```sh
idf.py update-dependencies
```

### Option C — Copy into your project's `components/` directory

```
your_project/
├── components/
│   └── esp-captive-portal/   ← copy this folder here
├── main/
└── CMakeLists.txt
```

ESP-IDF automatically discovers components in the `components/` directory at the project root.

---

## Quick Start

### 1. Configure the HTTP server

Set wildcard matching before calling `httpd_start()`:

- `uri_match_fn = httpd_uri_match_wildcard` — needed for `/browsernetworktime/*` and wildcard catch-all mode.

```c
#include "esp_http_server.h"
#include "captive_portal.h"

httpd_config_t config = HTTPD_DEFAULT_CONFIG();
config.uri_match_fn   = httpd_uri_match_wildcard; // required for probe wildcard matching

httpd_handle_t server = NULL;
httpd_start(&server, &config);
```

### 2. Register your app handlers, then call one function

Call in this order:

```c
// Step A: register your application handlers
httpd_register_uri_handler(server, &my_main_page);
httpd_register_uri_handler(server, &my_api_endpoint);

// Step B: one-line captive portal registration (call LAST)
captive_portal_register(server, NULL);
```

- **`captive_portal_register`** (single-call default):
  - Applies DHCP Option 114 first (when enabled in Kconfig).
  - Starts DNS lifecycle management automatically (AP start/stop).
  - Tries to register all 11 specific probe URIs.
  - If URI slots are insufficient (common with `HTTPD_DEFAULT_CONFIG()`), rolls back partial probe registration and automatically falls back to a 2-slot `/*` catch-all (GET + HEAD).

DHCP Option 114 and DNS/HTTP probing are complementary and non-conflicting.

> **Why call last?** If fallback catch-all mode is selected, `/*` must stay after your app handlers; otherwise it would intercept application routes.

---

## Configuration

### Runtime configuration (overrides Kconfig defaults)

```c
captive_portal_config_t portal_cfg = CAPTIVE_PORTAL_CONFIG_DEFAULT();

// Override specific fields as needed:
portal_cfg.redirect_url  = "http://10.0.0.1/";   // fixed URL (skips IP detection)
portal_cfg.netif_key     = "WIFI_AP_DEF";         // interface for IP detection
portal_cfg.redirect_port = 8080;                  // non-standard port

captive_portal_register(server, &portal_cfg);
```

Pass `NULL` instead of a config pointer to use all Kconfig compile-time defaults.

### Kconfig (menuconfig)

```
Component config → Captive Portal
  ├── Enable DHCP Option 114 captive portal URI      [*]
  ├── Network interface key for IP auto-detection  [WIFI_AP_DEF]
  ├── Fallback redirect IP address                 [192.168.4.1]
  └── Redirect target TCP port                     [80]
```

Open with:

```sh
idf.py menuconfig
```

---

## Advanced: Explicit Strategy Control

`captive_portal_register()` is the recommended API for almost all devices. It auto-selects specific-URI mode or catch-all mode based on remaining URI capacity.

Use explicit APIs only when you need strict manual control:

### A) Specific probe URI mode (11 handlers)

Use this if your device serves broader HTTP content and you want to avoid wildcard interception.

```c
httpd_register_uri_handler(server, &my_main_page);   // app handlers first
httpd_register_uri_handler(server, &my_api);
captive_portal_register_uris(server, NULL);          // probe URIs + DNS lifecycle last
```

### B) Wildcard catch-all mode (2 handlers)

Use this when URI slots are tight and your device only serves your own app routes.

Call **after** your application handlers so the wildcard does not shadow them:

```c
httpd_register_uri_handler(server, &my_main_page);
httpd_register_uri_handler(server, &my_api);
captive_portal_register_catchall(server, NULL);      // catchall + DNS lifecycle last
```

> **Note:** Avoid catch-all mode if your device serves content beyond your own application (for example, a general HTTP proxy or internet-facing server).

---

## API Reference

### `captive_portal_register()`

```c
esp_err_t captive_portal_register(httpd_handle_t server,
                                  const captive_portal_config_t *config);
```

| Parameter | Description |
|-----------|-------------|
| `server`  | Running `httpd_handle_t` from `httpd_start()`. Must not be NULL. |
| `config`  | Runtime configuration or NULL to use Kconfig defaults. |

Auto-selects HTTP strategy (specific URIs when capacity allows; otherwise catch-all), and starts DNS lifecycle management automatically.

**Returns:** `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_HTTPD_HANDLERS_FULL` (even fallback could not fit), or `ESP_FAIL`.

### `captive_portal_register_uris()`

```c
esp_err_t captive_portal_register_uris(httpd_handle_t server,
                                       const captive_portal_config_t *config);
```

Registers all 11 specific probe URI handlers **and** starts DNS lifecycle management. Equivalent to `captive_portal_register()` with an explicit specific-URI strategy. Call after application handlers.

| Parameter | Description |
|-----------|-------------|
| `server`  | Running `httpd_handle_t` from `httpd_start()`. Must not be NULL. |
| `config`  | Runtime configuration or NULL to use Kconfig defaults. |

**Returns:** `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_HTTPD_HANDLERS_FULL`, or `ESP_FAIL`.

### `captive_portal_register_catchall()`

```c
esp_err_t captive_portal_register_catchall(httpd_handle_t server,
                                           const captive_portal_config_t *config);
```

Registers wildcard `/*` handlers for GET and HEAD **and** starts DNS lifecycle management. Equivalent to `captive_portal_register()` with an explicit catch-all strategy. Must be called after all application URI handlers.

| Parameter | Description |
|-----------|-------------|
| `server`  | Running `httpd_handle_t` from `httpd_start()`. Must not be NULL. |
| `config`  | Runtime configuration or NULL to use Kconfig defaults. |

**Returns:** `ESP_OK`, `ESP_ERR_INVALID_ARG`, `ESP_ERR_HTTPD_HANDLERS_FULL`, or `ESP_FAIL`.

### `captive_portal_config_t`

| Field           | Type           | Description |
|-----------------|----------------|-------------|
| `redirect_url`  | `const char *` | Fixed redirect URL. NULL = auto-detect from `netif_key`. |
| `netif_key`     | `const char *` | esp_netif interface key. NULL = `CONFIG_CAPTIVE_PORTAL_NETIF_KEY`. |
| `redirect_port` | `uint16_t`     | Web server port. 0 = `CONFIG_CAPTIVE_PORTAL_REDIRECT_PORT`. Port 80 is omitted from URL. |

---

## Requirements

| Requirement | Detail |
|-------------|--------|
| ESP-IDF     | ≥ 5.0  |
| ESP32 target | esp32, esp32s2, esp32s3, esp32c3, esp32c6, esp32h2 |
| IDF components | `esp_http_server`, `esp_netif`, `esp_event`, `esp_wifi`, `freertos`, `lwip`, `log` |

---

## License

MIT License. See [LICENSE](LICENSE) for details.

---

## Changelog

### 1.3.0 (2026-04-15)
- Added DHCP Option 114 (RFC 8910) captive portal URI advertisement support
- Added Kconfig toggle `CAPTIVE_PORTAL_ENABLE_DHCP_OPTION_114` (default: enabled)
- DHCP Option 114 is now configured before HTTP URI handler registration in all public registration APIs

### 1.2.0 (2026-04-08)
- Added automatic registration strategy in `captive_portal_register()`: tries specific probe URIs first, then falls back atomically to catch-all when URI slots are insufficient
- Added `captive_portal_register_uris(server, config)` advanced API for explicit specific-URI registration; includes full DNS lifecycle management (was HTTP-only in initial implementation)
- Added `captive_portal_register_catchall(server, config)` — registers `/*` wildcard handlers (GET + HEAD) to redirect and immediately close any connection with an unmatched URI; includes full DNS lifecycle management; eliminates `ENFILE` socket exhaustion caused by background app traffic whose DNS resolves to the device IP
- All three public functions are now fully equivalent entry points: each applies configuration, registers HTTP handlers with its chosen strategy, and starts DNS — callers no longer need to combine multiple calls to get a working portal
- Updated recommended one-line integration flow: register app handlers first, then call `captive_portal_register()` last
- Refined API docs and examples to distinguish default automatic strategy from advanced explicit control
- `httpd_sess_trigger_close()` added to all redirect responses to free server socket slots immediately

### 1.1.0 (2026-04-07)
- Added built-in UDP DNS server (port 53) — all DNS queries answered with the AP IP address
- DNS server lifecycle fully automatic: starts on `WIFI_EVENT_AP_START`, stops on `WIFI_EVENT_AP_STOP`
- Fixed crash on rapid Wi-Fi stop/start: `WIFI_EVENT_AP_STOP` handler is now non-blocking
- Fixed socket double-close race condition between the DNS task and the stop signal path
- Added FreeRTOS mutex to serialise start/stop on concurrent AP state transitions
- Added `/generate204` (no underscore) Android Chrome probe variant to fix 404 retry storms


### 1.0.0 (2026-03-25)
- Initial release
- Handles iOS/macOS, Android, Windows, Firefox, and Chrome OS probes
- Single-function API with Kconfig and runtime configurability
- Automatic AP IP detection via esp_netif
