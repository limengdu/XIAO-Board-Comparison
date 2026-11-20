#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RENDER_W        320
#define RENDER_H        240
#define RENDER_SCENES   4
#define RENDER_MAX_ITER 255

typedef enum {
    RENDER_SEQ, /* one task renders the four images one after another / 一个任务依次渲染四张图 */
    RENDER_PAR, /* four tasks render one image each at the same time / 四个任务同时各渲染一张图 */
} render_mode_t;

/* Print chip, core count, CPU clock and firmware details as one "INFO {...}" line.
 * 以一行 "INFO {...}" 输出芯片、核心数、CPU 频率与固件信息。 */
void render_print_info(void);

/*
 * Render all four images `runs` times in `mode` and print one "RESULT {...}" line per run,
 * then "DONE {...}". Only computation is timed; pixels are reduced to a CRC32 per image.
 *
 * 以 `mode` 方式渲染四张图 `runs` 次，每次输出一行 "RESULT {...}"，最后输出 "DONE {...}"。
 * 只计时计算部分；像素只汇总为每张图的 CRC32。
 */
void render_bench(render_mode_t mode, uint32_t runs);

/* Render image `scene` untimed and print it as IMG_BEGIN / R <base64 row> / IMG_END lines.
 * 不计时渲染第 `scene` 张图，以 IMG_BEGIN / R <base64 行> / IMG_END 行输出。 */
void render_dump(uint32_t scene);
