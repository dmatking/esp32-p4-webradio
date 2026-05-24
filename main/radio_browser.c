// Copyright 2025 David M. King
// SPDX-License-Identifier: Apache-2.0
//
// Radio Browser API client
// Endpoint: https://de1.api.radio-browser.info/json/stations/search
// Docs: https://api.radio-browser.info/

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "wifi_prov.h"
#include "radio_browser.h"

#define TAG "radio_browser"

// Fixed part of the query: top MP3 stations by vote count, working only.
#define API_BASE \
    "https://de1.api.radio-browser.info/json/stations/search" \
    "?order=votes&reverse=true&hidebroken=true&codec=MP3"

// Built-in defaults, used when the captive-portal fields were never set (i.e.
// a unit provisioned before these fields existed — keeps the old behavior).
#define DEFAULT_COUNTRY "US"
#define DEFAULT_STATE   "Texas"
#define DEFAULT_LIMIT   60

#define RESP_BUF_SIZE (128 * 1024)  // 128 KB in PSRAM — Radio Browser returns ~70 KB for 60 stations

// Percent-encode `src` into `dst` (RFC 3986 unreserved set passes through).
static void url_encode(char *dst, size_t dlen, const char *src)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 3 < dlen; p++) {
        unsigned char c = *p;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[o++] = (char)c;
        } else {
            dst[o++] = '%';
            dst[o++] = hex[c >> 4];
            dst[o++] = hex[c & 0x0F];
        }
    }
    dst[o] = '\0';
}

// Assemble the search URL from the captive-portal settings (or defaults).
static void build_api_url(char *url, size_t ulen, int max_stations)
{
    char country[8], state[STATION_NAME_LEN], count[8];

    // wifi_prov_get returns false when the key was never stored — fall back to
    // the historical US/Texas/60 query. An explicitly-blank field means "no
    // filter" (worldwide / all states), so we keep the empty string.
    if (!wifi_prov_get("country", country, sizeof(country)))
        snprintf(country, sizeof(country), "%s", DEFAULT_COUNTRY);
    if (!wifi_prov_get("state", state, sizeof(state)))
        snprintf(state, sizeof(state), "%s", DEFAULT_STATE);

    int limit = DEFAULT_LIMIT;
    if (wifi_prov_get("count", count, sizeof(count)) && count[0])
        limit = atoi(count);
    if (limit < 1) limit = 1;
    if (limit > max_stations) limit = max_stations;

    int n = snprintf(url, ulen, "%s&limit=%d", API_BASE, limit);
    if (country[0] && n < (int)ulen)
        n += snprintf(url + n, ulen - n, "&countrycode=%s", country);
    if (state[0] && n < (int)ulen) {
        char enc[3 * STATION_NAME_LEN];
        url_encode(enc, sizeof(enc), state);
        snprintf(url + n, ulen - n, "&state=%s", enc);
    }
}

static char *s_resp_buf;
static int   s_resp_len;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (s_resp_len + evt->data_len < RESP_BUF_SIZE - 1) {
            memcpy(s_resp_buf + s_resp_len, evt->data, evt->data_len);
            s_resp_len += evt->data_len;
        } else {
            ESP_LOGW(TAG, "Response buffer full, truncating");
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

int radio_browser_fetch(radio_station_t *stations, int max_stations)
{
    s_resp_buf = heap_caps_malloc(RESP_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_resp_buf) {
        ESP_LOGE(TAG, "No memory for response buffer");
        return 0;
    }
    s_resp_len = 0;

    char api_url[512];
    build_api_url(api_url, sizeof(api_url), max_stations);
    ESP_LOGI(TAG, "Query: %s", api_url);

    esp_http_client_config_t config = {
        .url = api_url,
        .event_handler = http_event_handler,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
        .user_agent = "p4-radio/1.0",
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "HTTP request failed: err=%d status=%d", err, status);
        free(s_resp_buf);
        return 0;
    }

    s_resp_buf[s_resp_len] = '\0';
    ESP_LOGI(TAG, "Received %d bytes from Radio Browser API", s_resp_len);

    // Parse JSON array
    cJSON *root = cJSON_Parse(s_resp_buf);
    free(s_resp_buf);
    s_resp_buf = NULL;

    if (!root || !cJSON_IsArray(root)) {
        ESP_LOGE(TAG, "Failed to parse JSON response");
        cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    int total = cJSON_GetArraySize(root);
    for (int i = 0; i < total && count < max_stations; i++) {
        cJSON *item = cJSON_GetArrayItem(root, i);

        cJSON *name = cJSON_GetObjectItem(item, "name");
        cJSON *url  = cJSON_GetObjectItem(item, "url_resolved");
        cJSON *tags = cJSON_GetObjectItem(item, "tags");
        cJSON *br   = cJSON_GetObjectItem(item, "bitrate");

        // Skip stations without a usable stream URL
        if (!cJSON_IsString(url) || !url->valuestring || url->valuestring[0] == '\0')
            continue;
        if (!cJSON_IsString(name))
            continue;

        // Skip talk/news/politics stations
        const char *n = name->valuestring;
        const char *t = (cJSON_IsString(tags) && tags->valuestring) ? tags->valuestring : "";
        if (strcasestr(n, "infowars") || strcasestr(n, "NOAA") ||
            strcasestr(t, "news") || strcasestr(t, "politics") ||
            strcasestr(t, "weather"))
            continue;

        radio_station_t *s = &stations[count];
        snprintf(s->name,   sizeof(s->name),   "%s", name->valuestring);
        snprintf(s->url,    sizeof(s->url),     "%s", url->valuestring);
        snprintf(s->tags,   sizeof(s->tags),    "%s",
                 (cJSON_IsString(tags) && tags->valuestring) ? tags->valuestring : "");
        s->bitrate = cJSON_IsNumber(br) ? (uint32_t)br->valuedouble : 0;

        ESP_LOGI(TAG, "[%2d] %s (%u kbps)", count, s->name, s->bitrate);
        count++;
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "Fetched %d stations", count);
    return count;
}
