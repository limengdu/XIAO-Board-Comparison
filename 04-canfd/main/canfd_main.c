#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "bus_test.h"

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
    bus_print_info();
    return 0;
}

/*
 * run [mode=classic|fd_nobrs|fd] [bytes=N] [arb=BPS] [data=BPS] [run=N]
 * Defaults: mode=fd bytes=65536 arb=500000 data=2000000 run=1.
 *
 * 默认值：mode=fd、bytes=65536、arb=500000、data=2000000、run=1。
 */
static int cmd_run(int argc, char **argv)
{
    bus_run_cfg_t cfg = {
        .mode = BUS_FD,
        .arb_bps = 500000,
        .data_bps = 2000000,
        .bytes = 65536,
        .run = 1,
    };
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            return bad_arg(argv[i]);
        }
        *eq = '\0';
        const char *key = argv[i];
        const char *val = eq + 1;
        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "classic") == 0) {
                cfg.mode = BUS_CLASSIC;
            } else if (strcmp(val, "fd_nobrs") == 0) {
                cfg.mode = BUS_FD_NOBRS;
            } else if (strcmp(val, "fd") == 0) {
                cfg.mode = BUS_FD;
            } else {
                return bad_arg(key);
            }
        } else if (strcmp(key, "bytes") == 0) {
            if (!parse_u32(val, 64, BUS_MAX_BYTES, &cfg.bytes)) {
                return bad_arg(key);
            }
        } else if (strcmp(key, "arb") == 0) {
            if (!parse_u32(val, 50000, 1000000, &cfg.arb_bps)) {
                return bad_arg(key);
            }
        } else if (strcmp(key, "data") == 0) {
            if (!parse_u32(val, 500000, 5000000, &cfg.data_bps)) {
                return bad_arg(key);
            }
        } else if (strcmp(key, "run") == 0) {
            if (!parse_u32(val, 1, 1000, &cfg.run)) {
                return bad_arg(key);
            }
        } else {
            return bad_arg(key);
        }
    }
    if (cfg.mode != BUS_CLASSIC && cfg.data_bps < cfg.arb_bps) {
        return bad_arg("data");
    }
    bus_run(&cfg);
    return 0;
}

static int cmd_probe(int argc, char **argv)
{
    bus_probe();
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print chip, TWAI controllers and node pins",
            .func = cmd_info,
        },
        {
            .command = "run",
            .help = "Send one data block from node A to node B, e.g. run mode=classic|fd_nobrs|fd bytes=65536",
            .func = cmd_run,
        },
        {
            .command = "probe",
            .help = "Check each node's transceiver wiring on its own",
            .func = cmd_probe,
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
    repl_config.prompt = "canfd>";
    repl_config.task_stack_size = 6144;
    esp_console_register_help_command();
    register_commands();
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
