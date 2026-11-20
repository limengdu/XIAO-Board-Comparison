#include <stdio.h>
#include <string.h>
#include "app_info.h"
#include "esp_console.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "net.h"
#include "nvs_flash.h"
#include "storage.h"
#include "web.h"

static const char *TAG = "app";

static int cmd_info(int argc, char **argv)
{
    static char json[2048];
    if (app_info_json(json, sizeof(json)) == 0) {
        printf("\nINFO {\"error\":\"info_overflow\"}\n");
        return 1;
    }
    printf("\nINFO %s\n", json);
    return 0;
}

/* wifi <ssid> [password] */
static int cmd_wifi(int argc, char **argv)
{
    if (argc < 2 || argc > 3) {
        printf("\nRESULT {\"ok\":false,\"error\":\"usage: wifi <ssid> [password]\"}\n");
        return 1;
    }
    esp_err_t err = net_set_credentials(argv[1], argc == 3 ? argv[2] : "");
    printf("\nRESULT {\"ok\":%s,\"error\":\"%s\"}\n", err == ESP_OK ? "true" : "false", esp_err_to_name(err));
    return err == ESP_OK ? 0 : 1;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print board, firmware, flash layout, storage use and IP address",
            .func = cmd_info,
        },
        {
            .command = "wifi",
            .help = "Save Wi-Fi credentials and connect: wifi <ssid> [password]",
            .func = cmd_wifi,
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

    err = storage_mount();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "storage mount failed: %s", esp_err_to_name(err));
    }
    ESP_ERROR_CHECK(net_start());
    ESP_ERROR_CHECK(web_start());

    /* Boot reached the point where updates can be received again: keep this image.
     * 已能再次接收升级：确认保留当前固件。 */
    esp_ota_mark_app_valid_cancel_rollback();

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "flash>";
    repl_config.task_stack_size = 6144;
    esp_console_register_help_command();
    register_commands();
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
