#pragma once

#include <stdbool.h>
#include <stdint.h>

#define REC_SAMPLE_RATE_HZ   2000
#define REC_SAMPLE_BYTES     6
#define REC_SPIKE_AT_S       10
#define REC_SPIKE_SAMPLES    20
#define REC_DEFAULT_SECONDS  60
#define REC_MAX_SECONDS      600

typedef enum {
    REC_MEM_AUTO,     /* PSRAM when the board has it, otherwise internal RAM / 有 PSRAM 用 PSRAM，否则用内部 RAM */
    REC_MEM_INTERNAL, /* Internal RAM only / 只用内部 RAM */
} rec_mem_t;

/* Print board, memory and firmware details as one "INFO {...}" line.
 * 以一行 "INFO {...}" 输出板卡、内存与固件信息。 */
void rec_print_info(void);

/*
 * Record `seconds` of simulated 3-axis data in real time into a ring buffer, then print "RESULT {...}".
 * The buffer is sized for the full duration, limited by the chosen memory; when it is smaller,
 * the oldest samples are overwritten.
 *
 * 实时记录 `seconds` 秒模拟三轴数据到环形缓冲区，结束后输出 "RESULT {...}"。
 * 缓冲区按完整时长申请，受所选内存上限约束；放不下时覆盖最早的样本。
 */
void rec_record(uint32_t seconds, rec_mem_t mem);

/* Print the retained samples, oldest first, as DUMP_BEGIN / D <base64> / DUMP_END lines.
 * 按时间从早到晚，以 DUMP_BEGIN / D <base64> / DUMP_END 行输出保留的样本。 */
void rec_dump(void);
