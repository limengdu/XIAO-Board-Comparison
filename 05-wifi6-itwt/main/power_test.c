#include "power_test.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_event.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_private/esp_clk.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#ifndef SOC_WIFI_HE_SUPPORT
#define SOC_WIFI_HE_SUPPORT 0
#endif
#if SOC_WIFI_HE_SUPPORT
#include "esp_wifi_he.h"
#endif

#define NVS_NS              "pwr"
#define CONNECT_TIMEOUT_MS  30000
#define TWT_TIMEOUT_MS      6000
#define SETTLE_MS           3000    /* quiet time after a mode change before BEGIN / 切换方式后到 BEGIN 前的静置时间 */
#define REPORT_EVERY_S      10      /* uplink report period, same in every phase / 上行上报周期，各阶段相同 */
#define REPORT_BYTES        64
#define TWT_WAKE_DURA       255     /* nominal wake duration in 256 us units, as in the ESP-IDF example / 名义唤醒时长，单位 256 us，同 ESP-IDF 示例 */
#define TWT_INVL_EXPN       10
#define MAX_PHASES          12
#define MARKER_COPIES       3       /* each marker is sent this many times / 每条标记发送的次数 */
#define MARKER_GAP_MS       40

#define BIT_GOT_IP          BIT0
#define BIT_DISCONNECTED    BIT1
#define BIT_TWT_DONE        BIT2

typedef enum { MODE_PS, MODE_TWT } phase_mode_t;

typedef struct {
    bool band5;
    phase_mode_t mode;
    uint32_t twt_ms;
    uint8_t wake_units;     /* requested wake duration in 256 us units / 请求的唤醒时长，单位 256 us */
    uint8_t wake_ms;        /* wake duration from the plan, 0 = example default / 计划中的唤醒毫秒数，0 表示示例默认值 */
} phase_t;

static EventGroupHandle_t s_ev;
static esp_netif_t *s_netif;
static bool s_wifi_ready;
static int s_sock = -1;
static struct sockaddr_in s_host;
static volatile uint32_t s_pings;
#if SOC_WIFI_HE_SUPPORT
static wifi_event_sta_itwt_setup_t s_twt;
static bool s_twt_active;
#endif

/* ------------------------------------------------------------------ config */

static void get_str(nvs_handle_t h, const char *key, char *out, size_t len, const char *def)
{
    size_t n = len;
    if (nvs_get_str(h, key, out, &n) != ESP_OK) {
        strlcpy(out, def, len);
    }
}

void pt_config_load(pt_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->secs = 60;
    strlcpy(cfg->plan, "2g/ps", sizeof(cfg->plan));
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    get_str(h, "ssid", cfg->ssid, sizeof(cfg->ssid), "");
    get_str(h, "ssid5", cfg->ssid5, sizeof(cfg->ssid5), "");
    get_str(h, "pass", cfg->pass, sizeof(cfg->pass), "");
    get_str(h, "host", cfg->host, sizeof(cfg->host), "");
    get_str(h, "plan", cfg->plan, sizeof(cfg->plan), "2g/ps");
    nvs_get_u16(h, "secs", &cfg->secs);
    nvs_get_u8(h, "autorun", &cfg->autorun);
    nvs_close(h);
}

void pt_config_save(const pt_config_t *cfg)
{
    nvs_handle_t h;
    ESP_ERROR_CHECK(nvs_open(NVS_NS, NVS_READWRITE, &h));
    nvs_set_str(h, "ssid", cfg->ssid);
    nvs_set_str(h, "ssid5", cfg->ssid5);
    nvs_set_str(h, "pass", cfg->pass);
    nvs_set_str(h, "host", cfg->host);
    nvs_set_str(h, "plan", cfg->plan);
    nvs_set_u16(h, "secs", cfg->secs);
    nvs_set_u8(h, "autorun", cfg->autorun);
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
}

static uint8_t s_resets[PT_RESET_LOG];

void pt_record_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_resets);
    if (nvs_get_blob(h, "resets", s_resets, &len) != ESP_OK) {
        memset(s_resets, 0, sizeof(s_resets));
    }
    memmove(s_resets, s_resets + 1, sizeof(s_resets) - 1);
    s_resets[PT_RESET_LOG - 1] = (uint8_t)esp_reset_reason();
    nvs_set_blob(h, "resets", s_resets, sizeof(s_resets));
    nvs_commit(h);
    nvs_close(h);
}

static const char *reset_name(uint8_t r)
{
    switch (r) {
    case ESP_RST_POWERON:   return "poweron";
    case ESP_RST_SW:        return "sw";
    case ESP_RST_PANIC:     return "panic";
    case ESP_RST_INT_WDT:   return "int_wdt";
    case ESP_RST_TASK_WDT:  return "task_wdt";
    case ESP_RST_WDT:       return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_USB:       return "usb";
    case ESP_RST_UNKNOWN:   return "unknown";
    default:                return r ? "other" : "-";
    }
}

void pt_print_info(const pt_config_t *cfg)
{
    char resets[PT_RESET_LOG * 12] = "";
    for (int i = 0; i < PT_RESET_LOG; i++) {
        snprintf(resets + strlen(resets), sizeof(resets) - strlen(resets), "%s\"%s\"", i ? "," : "",
                 reset_name(s_resets[i]));
    }
    printf("\nRESETS {\"oldest_first\":[%s]}\n", resets);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    printf("\nINFO {\"target\":\"%s\",\"revision\":%u,\"cpu_mhz\":%d,\"xtal_mhz\":%d,\"idf\":\"%s\","
           "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"wifi6\":%s,\"band5\":%s,"
           "\"ssid\":\"%s\",\"ssid5\":\"%s\",\"pass_set\":%s,\"host\":\"%s\",\"plan\":\"%s\",\"secs\":%u,"
           "\"autorun\":%u}\n",
           CONFIG_IDF_TARGET, chip.revision, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_clk_xtal_freq() / 1000000,
           esp_get_idf_version(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           SOC_WIFI_HE_SUPPORT ? "true" : "false",
#if SOC_WIFI_SUPPORT_5G
           "true",
#else
           "false",
#endif
           cfg->ssid, cfg->ssid5, cfg->pass[0] ? "true" : "false", cfg->host, cfg->plan, cfg->secs, cfg->autorun);
}

/* ------------------------------------------------------------------ wifi */

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_ev, BIT_GOT_IP);
        xEventGroupSetBits(s_ev, BIT_DISCONNECTED);
#if SOC_WIFI_HE_SUPPORT
        s_twt_active = false;
#endif
    }
#if SOC_WIFI_HE_SUPPORT
    else if (id == WIFI_EVENT_ITWT_SETUP) {
        s_twt = *(wifi_event_sta_itwt_setup_t *)data;
        xEventGroupSetBits(s_ev, BIT_TWT_DONE);
    }
#endif
}

static void on_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    xEventGroupClearBits(s_ev, BIT_DISCONNECTED);
    xEventGroupSetBits(s_ev, BIT_GOT_IP);
}

/* Bring up the Wi-Fi station once; it stays initialised for the rest of the run.
 * 初始化一次 Wi-Fi station，之后保持初始化状态。 */
static void wifi_init_once(void)
{
    if (s_wifi_ready) {
        return;
    }
    s_ev = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
#if SOC_WIFI_HE_SUPPORT
    wifi_twt_config_t twt_cfg = { .post_wakeup_event = false, .twt_enable_keep_alive = false };
    ESP_ERROR_CHECK(esp_wifi_sta_twt_config(&twt_cfg));
#endif
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifi_ready = true;
}

static const char *phy_name(void)
{
    wifi_phy_mode_t phy;
    if (esp_wifi_sta_get_negotiated_phymode(&phy) != ESP_OK) {
        return "unknown";
    }
    switch (phy) {
    case WIFI_PHY_MODE_11B:
        return "11b";
    case WIFI_PHY_MODE_11G:
        return "11g";
    case WIFI_PHY_MODE_11A:
        return "11a";
    case WIFI_PHY_MODE_HT20:
        return "HT20";
    case WIFI_PHY_MODE_HT40:
        return "HT40";
    case WIFI_PHY_MODE_HE20:
        return "HE20";
    case WIFI_PHY_MODE_VHT20:
        return "VHT20";
    default:
        return "other";
    }
}

/* Connect on the requested band; returns true once an IP address is assigned.
 * 在指定频段连接；拿到 IP 地址后返回 true。 */
static bool wifi_connect(const pt_config_t *cfg, bool band5)
{
    if (xEventGroupGetBits(s_ev) & BIT_GOT_IP) {
        xEventGroupClearBits(s_ev, BIT_DISCONNECTED);
        esp_wifi_disconnect();
        xEventGroupWaitBits(s_ev, BIT_DISCONNECTED, pdFALSE, pdFALSE, pdMS_TO_TICKS(3000));
    }
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(band5 ? WIFI_BAND_MODE_5G_ONLY : WIFI_BAND_MODE_2G_ONLY);
#endif
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, band5 && cfg->ssid5[0] ? cfg->ssid5 : cfg->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, cfg->pass, sizeof(wc.sta.password));
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    for (int attempt = 0; attempt < 3; attempt++) {
        xEventGroupClearBits(s_ev, BIT_DISCONNECTED);
        esp_wifi_connect();
        EventBits_t bits = xEventGroupWaitBits(s_ev, BIT_GOT_IP | BIT_DISCONNECTED, pdFALSE, pdFALSE,
                                               pdMS_TO_TICKS(CONNECT_TIMEOUT_MS));
        if (bits & BIT_GOT_IP) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}

/* ------------------------------------------------------------------ udp */

/* Send one "TAG {...}" marker to the computer and echo it on the console.
 * 向电脑发送一条 "TAG {...}" 标记，并在串口回显。 */
static void marker(const char *tag, const char *fmt, ...)
{
    char body[480];
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    int n = snprintf(line, sizeof(line), "%s {%s}", tag, body);
    for (int i = 0; i < MARKER_COPIES && s_sock >= 0 && s_host.sin_addr.s_addr != 0; i++) {
        sendto(s_sock, line, n, 0, (struct sockaddr *)&s_host, sizeof(s_host));
        vTaskDelay(pdMS_TO_TICKS(MARKER_GAP_MS));
    }
    printf("\n%s\n", line);
}

/* Answer every "PING n" from the computer with "PONG n" straight away.
 * 收到电脑的 "PING n" 后立即回复 "PONG n"。 */
static void ping_task(void *arg)
{
    char buf[64];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(s_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &flen);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        buf[n] = '\0';
        if (strncmp(buf, "PING ", 5) == 0) {
            buf[1] = 'O';
            sendto(s_sock, buf, n, 0, (struct sockaddr *)&from, flen);
            s_pings++;
        }
    }
}

static bool udp_open(const pt_config_t *cfg)
{
    if (s_sock >= 0) {
        return true;
    }
    memset(&s_host, 0, sizeof(s_host));
    s_host.sin_family = AF_INET;
    s_host.sin_port = htons(PT_HOST_PORT);
    if (inet_pton(AF_INET, cfg->host, &s_host.sin_addr) != 1) {
        return false;
    }
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(PT_CMD_PORT), .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (s_sock < 0 || bind(s_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
        return false;
    }
    xTaskCreate(ping_task, "ping", 3072, NULL, 5, NULL);
    return true;
}

/* ------------------------------------------------------------------ phases */

static int parse_plan(const char *plan, phase_t *out)
{
    char buf[PT_PLAN_MAX];
    strlcpy(buf, plan, sizeof(buf));
    int n = 0;
    for (char *save, *tok = strtok_r(buf, ",", &save); tok && n < MAX_PHASES; tok = strtok_r(NULL, ",", &save)) {
        phase_t p = {0};
        if (strncmp(tok, "2g/", 3) == 0) {
            p.band5 = false;
        } else if (strncmp(tok, "5g/", 3) == 0) {
            p.band5 = true;
        } else {
            return -1;
        }
        const char *m = tok + 3;
        if (strcmp(m, "ps") == 0) {
            p.mode = MODE_PS;
        } else if (strncmp(m, "twt", 3) == 0 && atoi(m + 3) >= 50) {
            p.mode = MODE_TWT;
            p.twt_ms = (uint32_t)atoi(m + 3);
            const char *w = strchr(m + 3, 'w');
            p.wake_units = TWT_WAKE_DURA;
            if (w != NULL) {
                int wake_ms = atoi(w + 1);
                if (wake_ms < 1 || wake_ms > 65) {
                    return -1;
                }
                p.wake_units = (uint8_t)((wake_ms * 1000 + 255) / 256);
                p.wake_ms = (uint8_t)wake_ms;
            }
        } else {
            return -1;
        }
        out[n++] = p;
    }
    return n;
}

/* End the current TWT agreement, if any, and wait for queued frames to leave.
 * 结束当前 TWT 协议（如有），并等待排队的帧发出。 */
static void twt_teardown(void)
{
#if SOC_WIFI_HE_SUPPORT
    if (s_twt_active) {
        esp_wifi_sta_itwt_teardown(FLOW_ID_ALL);
        s_twt_active = false;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
#endif
}

/* Tear down any TWT agreement, then set up the one this phase needs. Writes a JSON fragment to `out`.
 * 拆除已有 TWT 协议，再按本阶段需要建立新协议；结果以 JSON 片段写入 `out`。 */
static bool apply_mode(const phase_t *p, char *out, size_t len)
{
#if SOC_WIFI_HE_SUPPORT
    twt_teardown();
    if (p->mode == MODE_PS) {
        snprintf(out, len, "\"twt\":null");
        return true;
    }
    uint32_t mant = p->twt_ms * 1000 >> TWT_INVL_EXPN;
    wifi_itwt_setup_config_t sc = {
        .setup_cmd = TWT_REQUEST,
        .trigger = 1,
        .flow_type = 0,
        .flow_id = 0,
        .wake_invl_expn = TWT_INVL_EXPN,
        .wake_duration_unit = 0,
        .min_wake_dura = p->wake_units,
        .wake_invl_mant = (uint16_t)mant,
        .twt_id = 0,
        .timeout_time_ms = 5000,
    };
    xEventGroupClearBits(s_ev, BIT_TWT_DONE);
    esp_err_t err = esp_wifi_sta_itwt_setup(&sc);
    bool done = err == ESP_OK &&
                (xEventGroupWaitBits(s_ev, BIT_TWT_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(TWT_TIMEOUT_MS)) & BIT_TWT_DONE);
    bool ok = done && s_twt.status == 1;
    s_twt_active = ok;
    const wifi_itwt_setup_config_t *c = &s_twt.config;
    snprintf(out, len,
             "\"twt\":{\"ok\":%s,\"api_err\":\"%s\",\"status\":%d,\"reason\":%u,\"wake_invl_us\":%" PRIu32 ","
             "\"wake_dura_us\":%u,\"trigger\":%u,\"announced\":%s,\"flow_id\":%u}",
             ok ? "true" : "false", esp_err_to_name(err), done ? (int)s_twt.status : -1, done ? s_twt.reason : 0,
             done ? ((uint32_t)c->wake_invl_mant << c->wake_invl_expn) : 0,
             done ? (unsigned)c->min_wake_dura << (c->wake_duration_unit ? 10 : 8) : 0,
             done ? c->trigger : 0, done && c->flow_type == 0 ? "true" : "false", done ? c->flow_id : 0);
    return ok;
#else
    if (p->mode == MODE_PS) {
        snprintf(out, len, "\"twt\":null");
        return true;
    }
    snprintf(out, len, "\"twt\":{\"ok\":false,\"api_err\":\"not_supported\"}");
    return false;
#endif
}

static void run_phase(int idx, const phase_t *p, uint16_t secs)
{
    char twt_json[256];
    bool ok = apply_mode(p, twt_json, sizeof(twt_json));
    vTaskDelay(pdMS_TO_TICKS(SETTLE_MS));

    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    esp_netif_ip_info_t ip = {0};
    esp_netif_get_ip_info(s_netif, &ip);
    char mode[24];
    if (p->mode == MODE_PS) {
        strlcpy(mode, "ps", sizeof(mode));
    } else if (p->wake_ms == 0) {
        snprintf(mode, sizeof(mode), "twt%" PRIu32, p->twt_ms);
    } else {
        snprintf(mode, sizeof(mode), "twt%" PRIu32 "w%u", p->twt_ms, p->wake_ms);
    }
    s_pings = 0;
    marker("BEGIN", "\"idx\":%d,\"band\":\"%s\",\"mode\":\"%s\",\"ok\":%s,%s,\"ip\":\"" IPSTR "\",\"channel\":%u,"
           "\"rssi\":%d,\"phy\":\"%s\",\"secs\":%u",
           idx, p->band5 ? "5g" : "2g", mode, ok ? "true" : "false", twt_json, IP2STR(&ip.ip), ap.primary, ap.rssi,
           phy_name(), secs);

    static uint8_t report[REPORT_BYTES];
    uint32_t reports = 0;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t s = 1; s <= secs; s++) {
        int64_t wake = t0 + (int64_t)s * 1000000;
        int64_t now = esp_timer_get_time();
        if (wake > now) {
            vTaskDelay(pdMS_TO_TICKS((wake - now) / 1000));
        }
        if (s % REPORT_EVERY_S == 0) {
            int n = snprintf((char *)report, sizeof(report), "REPORT {\"idx\":%d,\"n\":%" PRIu32 "}", idx, ++reports);
            memset(report + n, ' ', sizeof(report) - n);
            sendto(s_sock, report, sizeof(report), 0, (struct sockaddr *)&s_host, sizeof(s_host));
        }
    }
    bool connected = xEventGroupGetBits(s_ev) & BIT_GOT_IP;
    marker("END", "\"idx\":%d,\"pings\":%" PRIu32 ",\"reports\":%" PRIu32 ",\"connected\":%s",
           idx, s_pings, reports, connected ? "true" : "false");
}

void pt_run_plan(const pt_config_t *cfg, bool use_pm, bool deep_sleep_after)
{
    phase_t phases[MAX_PHASES];
    int n = parse_plan(cfg->plan, phases);
    if (n <= 0 || cfg->ssid[0] == '\0' || cfg->host[0] == '\0') {
        printf("\nDONE {\"ok\":false,\"error\":\"bad_config\"}\n");
        return;
    }
    if (use_pm) {
        esp_pm_config_t pm = {
            .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
            .min_freq_mhz = esp_clk_xtal_freq() / 1000000,
            .light_sleep_enable = true,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm));
    }
    wifi_init_once();
    int band = -1;
    for (int i = 0; i < n; i++) {
        if (phases[i].band5 != band || !(xEventGroupGetBits(s_ev) & BIT_GOT_IP)) {
            twt_teardown();
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (!wifi_connect(cfg, phases[i].band5)) {
                printf("\nDONE {\"ok\":false,\"error\":\"connect_failed\",\"band\":\"%s\"}\n",
                       phases[i].band5 ? "5g" : "2g");
                return;
            }
            band = phases[i].band5;
            if (!udp_open(cfg)) {
                printf("\nDONE {\"ok\":false,\"error\":\"bad_host\"}\n");
                return;
            }
            uint8_t mac[6];
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            marker("HELLO", "\"target\":\"%s\",\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"plan\":\"%s\",\"secs\":%u,"
                   "\"pm\":%s", CONFIG_IDF_TARGET, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], cfg->plan,
                   cfg->secs, use_pm ? "true" : "false");
        }
        run_phase(i, &phases[i], cfg->secs);
    }
    twt_teardown();
    vTaskDelay(pdMS_TO_TICKS(1000));
    marker("DONE", "\"ok\":true,\"phases\":%d", n);
    if (deep_sleep_after) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_wifi_stop();
        esp_deep_sleep_start();
    }
}

/* ------------------------------------------------------------------ scan */

void pt_scan(const pt_config_t *cfg)
{
    wifi_init_once();
#if SOC_WIFI_SUPPORT_5G
    esp_wifi_set_band_mode(WIFI_BAND_MODE_AUTO);
#endif
    wifi_scan_config_t sc = { .show_hidden = false };
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    uint16_t count = 0;
    esp_wifi_scan_get_ap_num(&count);
    wifi_ap_record_t *recs = calloc(count ? count : 1, sizeof(*recs));
    uint16_t got = count;
    if (recs != NULL) {
        esp_wifi_scan_get_ap_records(&got, recs);
        for (uint16_t i = 0; i < got; i++) {
            printf("AP {\"ssid\":\"%s\",\"channel\":%u,\"rssi\":%d,\"band\":\"%s\",\"ax\":%s}\n",
                   (const char *)recs[i].ssid, recs[i].primary, recs[i].rssi, recs[i].primary > 14 ? "5g" : "2g",
                   recs[i].phy_11ax ? "true" : "false");
        }
        free(recs);
    }
    printf("SCAN_DONE {\"ok\":%s,\"count\":%u}\n", err == ESP_OK ? "true" : "false", got);
}
