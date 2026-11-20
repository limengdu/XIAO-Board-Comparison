#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Start Wi-Fi in station mode and join the network saved in NVS, if any.
 * 以 STA 模式启动 Wi-Fi，若 NVS 中已保存网络则自动连接。 */
esp_err_t net_start(void);

/* Save credentials to NVS and reconnect with them.
 * 将 Wi-Fi 名称与密码保存到 NVS 并用它重新连接。 */
esp_err_t net_set_credentials(const char *ssid, const char *password);

/* Write the station IPv4 address into `buf` ("" while offline).
 * 将当前 IPv4 地址写入 `buf`（未联网时为空字符串）。 */
void net_ip(char *buf, size_t len);

/* RSSI of the current AP in dBm, 0 while offline.
 * 当前 AP 的信号强度（dBm），未联网时为 0。 */
int net_rssi(void);
