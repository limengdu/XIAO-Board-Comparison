#pragma once

#include <stdbool.h>
#include <stdint.h>

#define BUS_TX_GPIO_A   23      /* XIAO D4, sender TX / 发送节点 TX */
#define BUS_RX_GPIO_A   24      /* XIAO D5, sender RX / 发送节点 RX */
#define BUS_TX_GPIO_B   8       /* XIAO D8, receiver TX / 接收节点 TX */
#define BUS_RX_GPIO_B   9       /* XIAO D9, receiver RX / 接收节点 RX */

#define BUS_MAX_BYTES   (128 * 1024)

typedef enum {
    BUS_CLASSIC,    /* classic CAN, 8-byte frames / 经典 CAN，8 字节帧 */
    BUS_FD_NOBRS,   /* CAN FD 64-byte frames, whole frame at the arbitration rate / CAN FD 64 字节帧，全程仲裁速率 */
    BUS_FD,         /* CAN FD 64-byte frames, data phase at the data rate (BRS) / CAN FD 64 字节帧，数据段切换到数据速率 */
} bus_mode_t;

typedef struct {
    bus_mode_t mode;
    uint32_t arb_bps;       /* arbitration (and classic) bitrate / 仲裁段（及经典 CAN）速率 */
    uint32_t data_bps;      /* FD data-phase bitrate / FD 数据段速率 */
    uint32_t bytes;         /* size of the data block / 数据块大小 */
    uint32_t run;           /* run number echoed in the result / 结果中回显的轮次编号 */
} bus_run_cfg_t;

/* Print chip, controller pins and firmware details as one "INFO {...}" line.
 * 以一行 "INFO {...}" 输出芯片、控制器引脚与固件信息。 */
void bus_print_info(void);

/*
 * Send the same data block from node A to node B over the real bus and print one "RESULT {...}" line:
 * frames sent and received, missing / duplicate / out-of-order frames, CRC32 of both copies,
 * elapsed time from the first transmit to the last receive, and bus error counts.
 *
 * 通过真实总线把同一数据块从节点 A 发到节点 B，并输出一行 "RESULT {...}"：
 * 收发帧数、丢失 / 重复 / 乱序帧数、两份数据的 CRC32、从第一帧发出到最后一帧收到的耗时，以及总线错误计数。
 */
void bus_run(const bus_run_cfg_t *cfg);

/* Check each node's transceiver wiring on its own and print one "PROBE {...}" line.
 * 单独检查每个节点的收发器接线，并输出一行 "PROBE {...}"。 */
void bus_probe(void);
