#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PT_SSID_MAX     33
#define PT_PASS_MAX     65
#define PT_HOST_MAX     16
#define PT_PLAN_MAX     128
#define PT_CMD_PORT     5005    /* board listens for PING here / 板子在此端口接收 PING */
#define PT_HOST_PORT    5006    /* computer listens for phase markers here / 电脑在此端口接收阶段标记 */

typedef struct {
    char ssid[PT_SSID_MAX];     /* 2.4 GHz network, also used on 5 GHz when ssid5 is empty / 2.4 GHz 网络名，ssid5 为空时 5 GHz 也用它 */
    char ssid5[PT_SSID_MAX];
    char pass[PT_PASS_MAX];
    char host[PT_HOST_MAX];     /* IPv4 address of the computer / 电脑的 IPv4 地址 */
    char plan[PT_PLAN_MAX];     /* comma-separated phases, e.g. "2g/ps,2g/twt500,5g/twt2000w8" (w = wake ms) / 逗号分隔的测试阶段（w 为唤醒毫秒数） */
    uint16_t secs;              /* length of each phase in seconds / 每个阶段的秒数 */
    uint8_t autorun;            /* run the plan at power-up when no USB host is present / 未接 USB 主机时上电自动运行 */
} pt_config_t;

/* Load the configuration from NVS, filling defaults for missing keys.
 * 从 NVS 读取配置，缺失项填默认值。 */
void pt_config_load(pt_config_t *cfg);

/* Append this boot's reset reason to the last PT_RESET_LOG reasons kept in NVS; INFO prints them.
 * 把本次启动的复位原因追加到 NVS 中保存的最近 PT_RESET_LOG 条记录；INFO 会打印这些记录。 */
#define PT_RESET_LOG    8
void pt_record_reset(void);

/* Save the configuration to NVS.
 * 把配置写入 NVS。 */
void pt_config_save(const pt_config_t *cfg);

/* Print chip, Wi-Fi capabilities and the stored configuration (password hidden) as one "INFO {...}" line.
 * 以一行 "INFO {...}" 输出芯片、Wi-Fi 能力与已存配置（不显示密码）。 */
void pt_print_info(const pt_config_t *cfg);

/* Scan for access points and print one "AP {...}" line per network, then "SCAN_DONE {...}".
 * 扫描接入点，每个网络输出一行 "AP {...}"，最后输出 "SCAN_DONE {...}"。 */
void pt_scan(const pt_config_t *cfg);

/*
 * Run every phase in the plan: connect, set up the power-save mode, send BEGIN to the computer,
 * run the same network task for `secs` seconds, send END. Each marker is sent over UDP and also
 * printed to the console. With `use_pm` the chip scales its clock and enters automatic light sleep;
 * with `deep_sleep_after` it ends in deep sleep so the current trace shows a clear end point.
 *
 * 依次运行计划中的每个阶段：连接、设置省电方式、向电脑发送 BEGIN、运行 `secs` 秒相同的联网任务、发送 END。
 * 每个标记通过 UDP 发送并同时打印到串口。`use_pm` 为真时芯片降频并自动浅睡；
 * `deep_sleep_after` 为真时最后进入深睡，电流曲线上能看到明确的结束点。
 */
void pt_run_plan(const pt_config_t *cfg, bool use_pm, bool deep_sleep_after);
