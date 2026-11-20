#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "esp_console.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "power_test.h"

#define AUTORUN_CHECK_MS 1500   /* time for the USB host detection to settle / USB 主机检测稳定所需时间 */

static pt_config_t s_cfg;

static int bad_arg(const char *what)
{
    printf("\nRESULT {\"ok\":false,\"error\":\"bad_args\",\"detail\":\"%s\"}\n", what);
    return 1;
}

static bool parse_u32(const char *s, uint32_t lo, uint32_t hi, uint32_t *out)
{
    char *end;
    unsigned long v = strtoul(s, &end, 10);
    if (*s == '\0' || *end != '\0' || v < lo || v > hi) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static int cmd_info(int argc, char **argv)
{
    pt_print_info(&s_cfg);
    return 0;
}

/*
 * config key=value ...
 * Keys: ssid, ssid5, pass, host, plan, secs (10-600), autorun (0/1). Saved to NVS at once.
 *
 * 键：ssid、ssid5、pass、host、plan、secs（10-600）、autorun（0/1）。立即写入 NVS。
 */
static int cmd_config(int argc, char **argv)
{
    pt_config_t cfg = s_cfg;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            return bad_arg(argv[i]);
        }
        *eq = '\0';
        const char *key = argv[i];
        const char *val = eq + 1;
        uint32_t v;
        if (strcmp(key, "ssid") == 0 && strlen(val) < sizeof(cfg.ssid)) {
            strlcpy(cfg.ssid, val, sizeof(cfg.ssid));
        } else if (strcmp(key, "ssid5") == 0 && strlen(val) < sizeof(cfg.ssid5)) {
            strlcpy(cfg.ssid5, val, sizeof(cfg.ssid5));
        } else if (strcmp(key, "pass") == 0 && strlen(val) < sizeof(cfg.pass)) {
            strlcpy(cfg.pass, val, sizeof(cfg.pass));
        } else if (strcmp(key, "host") == 0 && strlen(val) < sizeof(cfg.host)) {
            strlcpy(cfg.host, val, sizeof(cfg.host));
        } else if (strcmp(key, "plan") == 0 && strlen(val) < sizeof(cfg.plan)) {
            strlcpy(cfg.plan, val, sizeof(cfg.plan));
        } else if (strcmp(key, "secs") == 0 && parse_u32(val, 10, 600, &v)) {
            cfg.secs = (uint16_t)v;
        } else if (strcmp(key, "autorun") == 0 && parse_u32(val, 0, 1, &v)) {
            cfg.autorun = (uint8_t)v;
        } else {
            return bad_arg(key);
        }
    }
    s_cfg = cfg;
    pt_config_save(&s_cfg);
    printf("\nRESULT {\"ok\":true}\n");
    return 0;
}

static int cmd_scan(int argc, char **argv)
{
    pt_scan(&s_cfg);
    return 0;
}

/*
 * start [pm=0|1]
 * Run the plan now from the console; pm=0 (default) keeps the CPU awake so USB stays usable.
 * pm=1 runs with light sleep as on battery; the board restarts at the end.
 *
 * 从串口立即运行计划；pm=0（默认）时 CPU 不进入浅睡，USB 保持可用。
 * pm=1 与电池供电时一样开启浅睡，结束后板子重启。
 */
static int cmd_start(int argc, char **argv)
{
    uint32_t pm = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "pm=", 3) != 0 || !parse_u32(argv[i] + 3, 0, 1, &pm)) {
            return bad_arg(argv[i]);
        }
    }
    pt_run_plan(&s_cfg, pm == 1, false);
    if (pm == 1) {
        /* Restart so the USB console comes back after light sleep.
         * 重启，使浅睡后的 USB 串口恢复。 */
        esp_restart();
    }
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print chip, Wi-Fi capabilities and the stored test configuration",
            .func = cmd_info,
        },
        {
            .command = "config",
            .help = "Store settings: config ssid=.. ssid5=.. pass=.. host=.. plan=2g/ps,2g/twt500 secs=60 autorun=1",
            .func = cmd_config,
        },
        {
            .command = "scan",
            .help = "List nearby access points with channel, band and Wi-Fi 6 support",
            .func = cmd_scan,
        },
        {
            .command = "start",
            .help = "Run the plan now: start [pm=0|1]",
            .func = cmd_start,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    pt_record_reset();
    pt_config_load(&s_cfg);

    /* Without a USB host (battery or power analyser) run the stored plan with light sleep; the console stays off.
     * 无 USB 主机（电池或功耗分析仪供电）时以浅睡方式运行已存计划，不启动串口命令行。 */
    if (s_cfg.autorun) {
        vTaskDelay(pdMS_TO_TICKS(AUTORUN_CHECK_MS));
        if (!usb_serial_jtag_is_connected()) {
            pt_run_plan(&s_cfg, true, true);
        }
    }

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "pwr>";
    repl_config.task_stack_size = 6144;
    esp_console_register_help_command();
    register_commands();
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
