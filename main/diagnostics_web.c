// SPDX-License-Identifier: MIT

#include "diagnostics_web.h"

#if CONFIG_BRIDGE_DIAGNOSTICS_WEB

#if !CONFIG_ESP_WIFI_ENABLE_WPA3_SAE || !CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
#error "Diagnostic SoftAP requires CONFIG_ESP_WIFI_ENABLE_WPA3_SAE and CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT"
#endif

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_spiffs.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "sdkconfig.h"

#define DIAG_AP_MAX_CLIENTS 2
#define DIAG_JSON_BUFFER_SIZE 768
#define DIAG_ASSET_BASE_PATH "/spiffs"
#define DIAG_ASSET_PARTITION_LABEL "storage"
#define DIAG_ASSET_CHUNK_SIZE 1024

static const char *TAG = "bridge_diag";
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_http_server;

static esp_err_t validate_password(void)
{
    const size_t len = strlen(CONFIG_BRIDGE_DIAGNOSTICS_AP_PASSWORD);
    ESP_RETURN_ON_FALSE(len >= 8 && len <= 63, ESP_ERR_INVALID_ARG, TAG,
                        "diagnostic AP WPA3 passphrase must be 8..63 characters");
    return ESP_OK;
}

static esp_err_t make_ap_ip_info(esp_netif_ip_info_t *info, dhcps_lease_t *lease)
{
    esp_ip4_addr_t ap_ip = {0};
    ESP_RETURN_ON_ERROR(esp_netif_str_to_ip4(CONFIG_BRIDGE_DIAGNOSTICS_AP_IP, &ap_ip), TAG,
                        "parse diagnostic AP IP address");

    const uint8_t a = esp_ip4_addr1(&ap_ip);
    const uint8_t b = esp_ip4_addr2(&ap_ip);
    const uint8_t c = esp_ip4_addr3(&ap_ip);
    const uint8_t d = esp_ip4_addr4(&ap_ip);
    ESP_RETURN_ON_FALSE(d != 0 && d != 255, ESP_ERR_INVALID_ARG, TAG, "diagnostic AP IP host octet is invalid");

    info->ip.addr = ap_ip.addr;
    info->gw.addr = ap_ip.addr;
    info->netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);

    uint8_t start = 2;
    uint8_t end = 20;
    if (d >= start && d <= end) {
        if (d != 2) {
            end = (uint8_t)(d - 1);
        } else {
            start = 3;
        }
    }
    ESP_RETURN_ON_FALSE(start <= end, ESP_ERR_INVALID_ARG, TAG, "cannot derive diagnostic AP DHCP range");

    lease->enable = true;
    lease->start_ip.addr = ESP_IP4TOADDR(a, b, c, start);
    lease->end_ip.addr = ESP_IP4TOADDR(a, b, c, end);
    return ESP_OK;
}

esp_err_t diagnostics_web_init_netif(void)
{
    s_ap_netif = esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_ERR_NO_MEM, TAG, "create default SoftAP netif");
    return ESP_OK;
}

esp_err_t diagnostics_web_configure_softap(void)
{
    ESP_RETURN_ON_ERROR(validate_password(), TAG, "validate diagnostic AP password");
    ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_ERR_INVALID_STATE, TAG, "diagnostic SoftAP netif not initialized");

    esp_netif_ip_info_t ip_info = {0};
    dhcps_lease_t lease = {0};
    ESP_RETURN_ON_ERROR(make_ap_ip_info(&ip_info, &lease), TAG, "prepare diagnostic AP IP info");

    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_RETURN_ON_ERROR(err, TAG, "stop SoftAP DHCP server");
    }
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(s_ap_netif, &ip_info), TAG, "set diagnostic AP IP info");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_REQUESTED_IP_ADDRESS, &lease,
                                               sizeof(lease)),
                        TAG, "set diagnostic AP DHCP lease range");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(s_ap_netif), TAG, "start SoftAP DHCP server");

    wifi_config_t ap_config = {0};
    strlcpy((char *)ap_config.ap.ssid, CONFIG_BRIDGE_DIAGNOSTICS_AP_SSID, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen((char *)ap_config.ap.ssid);
    strlcpy((char *)ap_config.ap.password, CONFIG_BRIDGE_DIAGNOSTICS_AP_PASSWORD, sizeof(ap_config.ap.password));
    ap_config.ap.channel = CONFIG_BRIDGE_WIFI_CHANNEL;
    ap_config.ap.authmode = WIFI_AUTH_WPA3_PSK;
    ap_config.ap.max_connection = DIAG_AP_MAX_CLIENTS;
    ap_config.ap.pmf_cfg.required = true;
    ap_config.ap.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap_config), TAG, "configure diagnostic SoftAP");
    ESP_LOGI(TAG, "Diagnostic WPA3 SoftAP '%s' on " IPSTR ", channel %d", CONFIG_BRIDGE_DIAGNOSTICS_AP_SSID,
             IP2STR(&ip_info.ip), CONFIG_BRIDGE_WIFI_CHANNEL);
    return ESP_OK;
}

static esp_err_t diagnostics_web_mount_assets(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = DIAG_ASSET_BASE_PATH,
        .partition_label = DIAG_ASSET_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err == ESP_ERR_INVALID_STATE) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "mount diagnostic web assets");

    size_t total = 0;
    size_t used = 0;
    err = esp_spiffs_info(DIAG_ASSET_PARTITION_LABEL, &total, &used);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Diagnostic web assets mounted from SPIFFS: %zu/%zu bytes used", used, total);
    } else {
        ESP_LOGW(TAG, "Diagnostic web assets mounted, but SPIFFS usage is unavailable: %s", esp_err_to_name(err));
    }

    return ESP_OK;
}

static const char *asset_path_for_uri(const char *uri)
{
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        return DIAG_ASSET_BASE_PATH "/index.html";
    }
    if (strcmp(uri, "/style.css") == 0) {
        return DIAG_ASSET_BASE_PATH "/style.css";
    }
    if (strcmp(uri, "/app.js") == 0) {
        return DIAG_ASSET_BASE_PATH "/app.js";
    }
    return NULL;
}

static const char *content_type_for_uri(const char *uri)
{
    if (strcmp(uri, "/") == 0 || strcmp(uri, "/index.html") == 0) {
        return "text/html; charset=utf-8";
    }
    if (strcmp(uri, "/style.css") == 0) {
        return "text/css; charset=utf-8";
    }
    if (strcmp(uri, "/app.js") == 0) {
        return "application/javascript; charset=utf-8";
    }
    return "application/octet-stream";
}

static esp_err_t static_asset_handler(httpd_req_t *req)
{
    const char *path = asset_path_for_uri(req->uri);
    if (path == NULL) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        ESP_LOGW(TAG, "Diagnostic web asset not found: %s", path);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
    }

    httpd_resp_set_type(req, content_type_for_uri(req->uri));
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char buffer[DIAG_ASSET_CHUNK_SIZE];
    esp_err_t err = ESP_OK;
    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        err = httpd_resp_send_chunk(req, buffer, read_bytes);
        if (err != ESP_OK) {
            break;
        }
    }

    fclose(file);

    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);
    }
    return err;
}

static void mac_to_string(const uint8_t mac[ESP_NOW_ETH_ALEN], char out[18])
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    bridge_status_t status = {0};
    bridge_get_status(&status);

    char own_mac[18];
    char peer_mac[18];
    mac_to_string(status.own_mac, own_mac);
    mac_to_string(status.peer_mac, peer_mac);

    char json[DIAG_JSON_BUFFER_SIZE];
    int len = snprintf(json, sizeof(json),
                       "{\"uptime_ms\":%" PRIu64 ",\"own_mac\":\"%s\",\"peer_mac\":\"%s\",\"channel\":%d,"
                       "\"rx_received_packets\":%u,\"rx_received_bytes\":%u,\"rx_dropped_bytes\":%u,"
                       "\"rx_truncated_bytes\":%u,\"send_success_count\":%u,\"send_fail_count\":%u,"
                       "\"tx_dropped_packets\":%u,\"tx_dropped_bytes\":%u}",
                       status.uptime_ms, own_mac, peer_mac, status.channel, status.rx_received_packets,
                       status.rx_received_bytes, status.rx_dropped_bytes, status.rx_truncated_bytes,
                       status.send_success_count, status.send_fail_count, status.tx_dropped_packets,
                       status.tx_dropped_bytes);
    if (len < 0 || len >= (int)sizeof(json)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status JSON truncated");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, json, len);
}

esp_err_t diagnostics_web_start_http(void)
{
    if (s_http_server != NULL) {
        return ESP_OK;
    }

    ESP_RETURN_ON_ERROR(diagnostics_web_mount_assets(), TAG, "mount diagnostic web assets");

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 5;
    config.max_open_sockets = 3;
    config.lru_purge_enable = true;
    config.stack_size = 4096;
    config.task_priority = 3;

    ESP_RETURN_ON_ERROR(httpd_start(&s_http_server, &config), TAG, "start diagnostic HTTP server");

    const httpd_uri_t root_uri = {.uri = "/", .method = HTTP_GET, .handler = static_asset_handler};
    const httpd_uri_t index_uri = {.uri = "/index.html", .method = HTTP_GET, .handler = static_asset_handler};
    const httpd_uri_t style_uri = {.uri = "/style.css", .method = HTTP_GET, .handler = static_asset_handler};
    const httpd_uri_t app_uri = {.uri = "/app.js", .method = HTTP_GET, .handler = static_asset_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &root_uri), TAG, "register diagnostic root handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &index_uri), TAG, "register diagnostic index handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &style_uri), TAG, "register diagnostic style handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &app_uri), TAG, "register diagnostic app handler");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_http_server, &status_uri), TAG, "register diagnostic status handler");
    ESP_LOGI(TAG, "Diagnostic web interface ready at http://%s/", CONFIG_BRIDGE_DIAGNOSTICS_AP_IP);
    return ESP_OK;
}

#endif
