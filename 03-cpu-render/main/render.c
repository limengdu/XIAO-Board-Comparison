#include "render.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define FRAC        24                          /* Q7.24 fixed point / Q7.24 定点数 */
#define Q(v)        ((int32_t)((v) * (1 << FRAC)))
#define ESCAPE      (4 << FRAC)
#define TASK_STACK  4096
#define TASK_PRIO   1
#define DUMP_YIELD  8                           /* rows between scheduler yields / 每输出多少行让出一次 CPU */

typedef struct {
    const char *name;
    bool julia;
    int32_t x0, y0;   /* top-left corner / 左上角坐标 */
    int32_t step;     /* distance between pixels / 相邻像素间距 */
    int32_t jx, jy;   /* Julia constant / Julia 常数 */
} scene_t;

/* Corner = center - span / 2, step = span / width; all values are exact integers on every chip.
 * 左上角 = 中心 - 跨度 / 2，步长 = 跨度 / 宽度；所有数值在各芯片上都是相同的整数。 */
#define SCENE(n, j, cx, cy, span, jxv, jyv)                                                        \
    { .name = n, .julia = j, .step = Q(span) / RENDER_W,                                           \
      .x0 = Q(cx) - (Q(span) / RENDER_W) * (RENDER_W / 2),                                         \
      .y0 = Q(cy) - (Q(span) / RENDER_W) * (RENDER_H / 2), .jx = Q(jxv), .jy = Q(jyv) }

static const scene_t s_scenes[RENDER_SCENES] = {
    SCENE("mandelbrot", false, -0.5, 0.0, 3.0, 0, 0),
    SCENE("seahorse-valley", false, -0.7436, 0.1318, 0.02, 0, 0),
    SCENE("elephant-valley", false, 0.2925, 0.0159, 0.03, 0, 0),
    SCENE("julia", true, 0.0, 0.0, 3.2, -0.8, 0.156),
};

typedef struct {
    uint32_t scene;
    int64_t us;
    uint32_t crc;
    uint64_t iters;
} job_t;

static EventGroupHandle_t s_done;

/* Escape-time iteration count for one point, RENDER_MAX_ITER when it does not escape.
 * 单个点的逃逸迭代次数；不逃逸时返回 RENDER_MAX_ITER。 */
static inline uint32_t iterate(int32_t x, int32_t y, int32_t cx, int32_t cy)
{
    for (uint32_t i = 0; i < RENDER_MAX_ITER; i++) {
        int32_t x2 = (int32_t)(((int64_t)x * x) >> FRAC);
        int32_t y2 = (int32_t)(((int64_t)y * y) >> FRAC);
        if (x2 + y2 > ESCAPE) {
            return i;
        }
        y = (int32_t)(((int64_t)x * y) >> (FRAC - 1)) + cy;
        x = x2 - y2 + cx;
    }
    return RENDER_MAX_ITER;
}

/* Fill `out` with one row of iteration counts; returns the sum of the counts.
 * 用一行迭代次数填充 `out`；返回该行迭代次数之和。 */
static uint32_t render_row(const scene_t *s, uint32_t row, uint8_t *out)
{
    uint32_t sum = 0;
    int32_t y = s->y0 + (int32_t)row * s->step;
    for (uint32_t col = 0; col < RENDER_W; col++) {
        int32_t x = s->x0 + (int32_t)col * s->step;
        uint32_t n = s->julia ? iterate(x, y, s->jx, s->jy) : iterate(0, 0, x, y);
        out[col] = (uint8_t)n;
        sum += n;
    }
    return sum;
}

static void render_job(job_t *job)
{
    uint8_t row[RENDER_W];
    const scene_t *s = &s_scenes[job->scene];
    uint32_t crc = 0;
    uint64_t iters = 0;
    int64_t t0 = esp_timer_get_time();
    for (uint32_t r = 0; r < RENDER_H; r++) {
        iters += render_row(s, r, row);
        crc = esp_rom_crc32_le(crc, row, sizeof(row));
    }
    job->us = esp_timer_get_time() - t0;
    job->crc = crc;
    job->iters = iters;
}

static void par_task(void *arg)
{
    job_t *job = arg;
    render_job(job);
    xEventGroupSetBits(s_done, BIT(job->scene));
    vTaskDelete(NULL);
}

static void seq_task(void *arg)
{
    job_t *jobs = arg;
    for (uint32_t i = 0; i < RENDER_SCENES; i++) {
        render_job(&jobs[i]);
    }
    xEventGroupSetBits(s_done, BIT(RENDER_SCENES));
    vTaskDelete(NULL);
}

void render_print_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("\nINFO {\"target\":\"%s\",\"revision\":%u,\"cores\":%u,\"cpu_mhz\":%d,\"idf\":\"%s\","
           "\"width\":%d,\"height\":%d,\"max_iter\":%d,\"scenes\":[",
           CONFIG_IDF_TARGET, chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version(),
           RENDER_W, RENDER_H, RENDER_MAX_ITER);
    for (uint32_t i = 0; i < RENDER_SCENES; i++) {
        printf("%s\"%s\"", i ? "," : "", s_scenes[i].name);
    }
    printf("]}\n");
}

void render_bench(render_mode_t mode, uint32_t runs)
{
    if (s_done == NULL) {
        s_done = xEventGroupCreate();
    }
    const char *mode_name = mode == RENDER_SEQ ? "seq" : "par";
    for (uint32_t run = 1; run <= runs; run++) {
        job_t jobs[RENDER_SCENES];
        for (uint32_t i = 0; i < RENDER_SCENES; i++) {
            jobs[i] = (job_t){ .scene = i };
        }
        xEventGroupClearBits(s_done, 0xff);
        EventBits_t wait;
        int64_t t0 = esp_timer_get_time();
        if (mode == RENDER_SEQ) {
            wait = BIT(RENDER_SCENES);
            xTaskCreate(seq_task, "render", TASK_STACK, jobs, TASK_PRIO, NULL);
        } else {
            wait = BIT(RENDER_SCENES) - 1;
            for (uint32_t i = 0; i < RENDER_SCENES; i++) {
                xTaskCreate(par_task, "render", TASK_STACK, &jobs[i], TASK_PRIO, NULL);
            }
        }
        xEventGroupWaitBits(s_done, wait, pdTRUE, pdTRUE, portMAX_DELAY);
        int64_t wall_us = esp_timer_get_time() - t0;

        uint64_t iters = 0;
        printf("\nRESULT {\"ok\":true,\"mode\":\"%s\",\"run\":%" PRIu32 ",\"wall_us\":%" PRId64 ",\"images\":[",
               mode_name, run, wall_us);
        for (uint32_t i = 0; i < RENDER_SCENES; i++) {
            iters += jobs[i].iters;
            printf("%s{\"scene\":\"%s\",\"us\":%" PRId64 ",\"crc32\":%" PRIu32 ",\"iters\":%" PRIu64 "}",
                   i ? "," : "", s_scenes[i].name, jobs[i].us, jobs[i].crc, jobs[i].iters);
        }
        printf("],\"iters\":%" PRIu64 "}\n", iters);
        /* Let the idle task free the finished tasks before the next run.
         * 下一轮开始前让空闲任务回收已结束的任务。 */
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    printf("DONE {\"mode\":\"%s\",\"runs\":%" PRIu32 "}\n", mode_name, runs);
}

static void base64_encode(const uint8_t *in, size_t len, char *out)
{
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[o++] = tbl[(v >> 18) & 0x3f];
        out[o++] = tbl[(v >> 12) & 0x3f];
        out[o++] = i + 1 < len ? tbl[(v >> 6) & 0x3f] : '=';
        out[o++] = i + 2 < len ? tbl[v & 0x3f] : '=';
    }
    out[o] = '\0';
}

void render_dump(uint32_t scene)
{
    if (scene >= RENDER_SCENES) {
        printf("\nIMG_END {\"ok\":false,\"error\":\"bad_scene\"}\n");
        return;
    }
    static char line[(RENDER_W + 2) / 3 * 4 + 1];
    uint8_t row[RENDER_W];
    uint32_t crc = 0;
    printf("\nIMG_BEGIN {\"scene\":\"%s\",\"width\":%d,\"height\":%d}\n", s_scenes[scene].name, RENDER_W, RENDER_H);
    for (uint32_t r = 0; r < RENDER_H; r++) {
        render_row(&s_scenes[scene], r, row);
        crc = esp_rom_crc32_le(crc, row, sizeof(row));
        base64_encode(row, sizeof(row), line);
        printf("R %s\n", line);
        if ((r + 1) % DUMP_YIELD == 0) {
            vTaskDelay(1);
        }
    }
    printf("IMG_END {\"ok\":true,\"crc32\":%" PRIu32 "}\n", crc);
    fflush(stdout);
}
