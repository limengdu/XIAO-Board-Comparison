#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "recorder.h"

static int bad_arg(const char *what)
{
    printf("\nRESULT {\"ok\":false,\"error\":\"bad_args\",\"detail\":\"%s\"}\n", what);
    return 1;
}

static int cmd_info(int argc, char **argv)
{
    rec_print_info();
    return 0;
}

/*
 * record [seconds=N] [mem=auto|internal]
 * Defaults: seconds=60 mem=auto.
 *
 * 默认值：seconds=60、mem=auto。
 */
static int cmd_record(int argc, char **argv)
{
    uint32_t seconds = REC_DEFAULT_SECONDS;
    rec_mem_t mem = REC_MEM_AUTO;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            return bad_arg(argv[i]);
        }
        *eq = '\0';
        const char *key = argv[i];
        const char *val = eq + 1;
        if (strcmp(key, "seconds") == 0) {
            char *end;
            unsigned long v = strtoul(val, &end, 10);
            if (*val == '\0' || *end != '\0' || v < REC_SPIKE_AT_S + 1 || v > REC_MAX_SECONDS) {
                return bad_arg(key);
            }
            seconds = (uint32_t)v;
        } else if (strcmp(key, "mem") == 0) {
            if (strcmp(val, "auto") == 0) {
                mem = REC_MEM_AUTO;
            } else if (strcmp(val, "internal") == 0) {
                mem = REC_MEM_INTERNAL;
            } else {
                return bad_arg(key);
            }
        } else {
            return bad_arg(key);
        }
    }
    rec_record(seconds, mem);
    return 0;
}

static int cmd_dump(int argc, char **argv)
{
    rec_dump();
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print board, memory and firmware details",
            .func = cmd_info,
        },
        {
            .command = "record",
            .help = "Record simulated 3-axis data in real time, e.g. record seconds=60 mem=auto|internal",
            .func = cmd_record,
        },
        {
            .command = "dump",
            .help = "Print the retained samples as base64 lines",
            .func = cmd_dump,
        },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void app_main(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "rec>";
    repl_config.task_stack_size = 8192;
    esp_console_register_help_command();
    register_commands();
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
