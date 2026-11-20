#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_console.h"
#include "render.h"

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
    render_print_info();
    return 0;
}

/*
 * bench [mode=seq|par] [runs=N]
 * Defaults: mode=seq runs=5.
 *
 * 默认值：mode=seq、runs=5。
 */
static int cmd_bench(int argc, char **argv)
{
    render_mode_t mode = RENDER_SEQ;
    uint32_t runs = 5;
    for (int i = 1; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        if (eq == NULL) {
            return bad_arg(argv[i]);
        }
        *eq = '\0';
        const char *key = argv[i];
        const char *val = eq + 1;
        if (strcmp(key, "mode") == 0) {
            if (strcmp(val, "seq") == 0) {
                mode = RENDER_SEQ;
            } else if (strcmp(val, "par") == 0) {
                mode = RENDER_PAR;
            } else {
                return bad_arg(key);
            }
        } else if (strcmp(key, "runs") == 0) {
            if (!parse_u32(val, 1, 50, &runs)) {
                return bad_arg(key);
            }
        } else {
            return bad_arg(key);
        }
    }
    render_bench(mode, runs);
    return 0;
}

/* dump <scene index 0-3> */
static int cmd_dump(int argc, char **argv)
{
    uint32_t scene;
    if (argc != 2 || !parse_u32(argv[1], 0, RENDER_SCENES - 1, &scene)) {
        printf("\nIMG_END {\"ok\":false,\"error\":\"usage: dump <0-3>\"}\n");
        return 1;
    }
    render_dump(scene);
    return 0;
}

static void register_commands(void)
{
    const esp_console_cmd_t cmds[] = {
        {
            .command = "info",
            .help = "Print chip, cores, CPU clock and render settings",
            .func = cmd_info,
        },
        {
            .command = "bench",
            .help = "Render the four images and time it, e.g. bench mode=seq|par runs=5",
            .func = cmd_bench,
        },
        {
            .command = "dump",
            .help = "Render one image untimed and print its pixels: dump <0-3>",
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
    repl_config.prompt = "cpu>";
    repl_config.task_stack_size = 6144;
    esp_console_register_help_command();
    register_commands();
    ESP_ERROR_CHECK(esp_console_new_repl_stdio(&repl_config, &repl));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
