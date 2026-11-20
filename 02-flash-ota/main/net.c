#include "net.h"

#include <stdio.h>
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"

#define NVS_NS "wifi"

static const char *TAG = "net";
static esp_netif_t *s_netif;
static esp_ip4_addr_t s_ip;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_ip.addr = 0;
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        s_ip = ((ip_event_got_ip_t *)data)->ip_info.ip;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&s_ip));
    }
}

static bool load_config(wifi_config_t *cfg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t ssid_len = sizeof(cfg->sta.ssid);
    size_t pass_len = sizeof(cfg->sta.password);
    bool ok = nvs_get_str(h, "ssid", (char *)cfg->sta.ssid, &ssid_len) == ESP_OK &&
              nvs_get_str(h, "pass", (char *)cfg->sta.password, &pass_len) == ESP_OK;
    nvs_close(h);
    return ok;
}

static esp_err_t apply_config(const wifi_config_t *saved)
{
    wifi_config_t cfg = *saved;
    cfg.sta.threshold.authmode = cfg.sta.password[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    cfg.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    return esp_wifi_connect();
}

esp_err_t net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    wifi_config_t cfg = { 0 };
    return load_config(&cfg) ? apply_config(&cfg) : ESP_OK;
}

esp_err_t net_set_credentials(const char *ssid, const char *password)
{
    wifi_config_t cfg = { 0 };
    if (strlen(ssid) >= sizeof(cfg.sta.ssid) || strlen(password) >= sizeof(cfg.sta.password)) {
        return ESP_ERR_INVALID_ARG;
    }
    strcpy((char *)cfg.sta.ssid, ssid);
    strcpy((char *)cfg.sta.password, password);

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(h, "ssid", ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "pass", password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK ? apply_config(&cfg) : err;
}

void net_ip(char *buf, size_t len)
{
    if (s_ip.addr == 0) {
        buf[0] = '\0';
    } else {
        snprintf(buf, len, IPSTR, IP2STR(&s_ip));
    }
}

int net_rssi(void)
{
    wifi_ap_record_t ap;
    return (s_ip.addr != 0 && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) ? ap.rssi : 0;
}
