#include "recorder.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_mac.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#define INTERNAL_CAPS        (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define INTERNAL_RESERVE     (32 * 1024)
#define HEAP_BLOCK_OVERHEAD  64
#define PROGRESS_EVERY_S     5
#define DUMP_CHUNK_SAMPLES   128
#define DUMP_YIELD_EVERY     16   /* lines between scheduler yields / 每输出多少行让出一次 CPU */
#define X_PERIOD             40   /* 50 Hz at 2 kHz / 2 kHz 采样下的 50 Hz */
#define Y_PERIOD             50   /* 3 cycles of 120 Hz at 2 kHz / 2 kHz 采样下 120 Hz 的 3 个周期 */

typedef struct {
    uint8_t *buf;
    size_t capacity;          /* samples the buffer holds / 缓冲区可容纳的样本数 */
    uint64_t written;         /* samples generated so far / 已生成的样本总数 */
    uint32_t seconds;
    bool psram;
    int64_t elapsed_us;
} rec_state_t;

static rec_state_t s_rec;
static int16_t s_x_table[X_PERIOD];
static int16_t s_y_table[Y_PERIOD];
static bool s_tables_ready;

static void init_tables(void)
{
    for (int i = 0; i < X_PERIOD; i++) {
        s_x_table[i] = (int16_t)lrintf(1000.0f * sinf(2.0f * (float)M_PI * i / X_PERIOD));
    }
    for (int i = 0; i < Y_PERIOD; i++) {
        s_y_table[i] = (int16_t)lrintf(600.0f * sinf(2.0f * (float)M_PI * 3.0f * i / Y_PERIOD));
    }
    s_tables_ready = true;
}

static uint32_t xorshift32(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/*
 * Write sample `n` into `out` as little-endian int16 x, y, z.
 * X: 50 Hz vibration, Y: 120 Hz vibration, Z: 1 g at 4096 LSB/g; all with small noise.
 * A 10 ms burst of large values starts at REC_SPIKE_AT_S.
 *
 * 将第 `n` 个样本以小端 int16 x、y、z 写入 `out`。
 * X 为 50 Hz 振动，Y 为 120 Hz 振动，Z 为 1 g（4096 LSB/g），均叠加少量噪声；
 * 从 REC_SPIKE_AT_S 开始有一段 10 ms 的大幅值尖峰。
 */
static void make_sample(uint64_t n, uint32_t *noise, uint8_t *out)
{
    int16_t x = s_x_table[n % X_PERIOD] + (int16_t)(xorshift32(noise) % 129) - 64;
    int16_t y = s_y_table[n % Y_PERIOD] + (int16_t)(xorshift32(noise) % 129) - 64;
    int16_t z = 4096 + (int16_t)(xorshift32(noise) % 65) - 32;
    uint64_t spike_start = (uint64_t)REC_SPIKE_AT_S * REC_SAMPLE_RATE_HZ;
    if (n >= spike_start && n < spike_start + REC_SPIKE_SAMPLES) {
        int16_t sign = ((n - spike_start) & 1) ? -1 : 1;
        x = sign * 20000;
        y = -sign * 15000;
        z = 4096 + sign * 12000;
    }
    int16_t v[3] = { x, y, z };
    memcpy(out, v, sizeof(v));
}

/* Allocate the ring buffer; returns the size in bytes, 0 on failure.
 * 申请环形缓冲区；返回字节数，失败返回 0。 */
static size_t alloc_buffer(size_t want, rec_mem_t mem)
{
    s_rec.psram = false;
    if (mem == REC_MEM_AUTO && heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        s_rec.buf = heap_caps_malloc(want, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_rec.buf != NULL) {
            s_rec.psram = true;
            return want;
        }
    }
    size_t largest = heap_caps_get_largest_free_block(INTERNAL_CAPS);
    size_t free_total = heap_caps_get_free_size(INTERNAL_CAPS);
    size_t size = want;
    if (largest > HEAP_BLOCK_OVERHEAD && size > largest - HEAP_BLOCK_OVERHEAD) {
        size = largest - HEAP_BLOCK_OVERHEAD;
    }
    if (free_total > INTERNAL_RESERVE && size > free_total - INTERNAL_RESERVE) {
        size = free_total - INTERNAL_RESERVE;
    }
    size -= size % REC_SAMPLE_BYTES;
    s_rec.buf = size ? heap_caps_malloc(size, INTERNAL_CAPS) : NULL;
    return s_rec.buf ? size : 0;
}

static uint64_t retained(void)
{
    return s_rec.written < s_rec.capacity ? s_rec.written : s_rec.capacity;
}

void rec_print_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    printf("\nINFO {\"target\":\"%s\",\"revision\":%u,\"cores\":%u,\"cpu_mhz\":%d,"
           "\"mac\":\"%02x:%02x:%02x:%02x:%02x:%02x\",\"idf\":\"%s\","
           "\"psram_total\":%u,\"psram_free\":%u,\"internal_free\":%u,\"internal_largest\":%u}\n",
           CONFIG_IDF_TARGET, chip.revision, chip.cores, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], esp_get_idf_version(),
           (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_free_size(INTERNAL_CAPS),
           (unsigned)heap_caps_get_largest_free_block(INTERNAL_CAPS));
}

void rec_record(uint32_t seconds, rec_mem_t mem)
{
    if (!s_tables_ready) {
        init_tables();
    }
    heap_caps_free(s_rec.buf);
    memset(&s_rec, 0, sizeof(s_rec));
    s_rec.seconds = seconds;

    size_t want = (size_t)seconds * REC_SAMPLE_RATE_HZ * REC_SAMPLE_BYTES;
    size_t bytes = alloc_buffer(want, mem);
    if (bytes == 0) {
        printf("\nRESULT {\"ok\":false,\"error\":\"alloc_failed\",\"wanted_bytes\":%u}\n", (unsigned)want);
        return;
    }
    s_rec.capacity = bytes / REC_SAMPLE_BYTES;
    printf("\nSTART {\"seconds\":%" PRIu32 ",\"memory\":\"%s\",\"buffer_bytes\":%u,\"wanted_bytes\":%u,"
           "\"capacity_s\":%.3f}\n",
           seconds, s_rec.psram ? "psram" : "internal", (unsigned)bytes, (unsigned)want,
           (double)s_rec.capacity / REC_SAMPLE_RATE_HZ);

    const uint64_t total = (uint64_t)seconds * REC_SAMPLE_RATE_HZ;
    uint32_t noise = 0x1234567u;
    uint32_t next_report_s = PROGRESS_EVERY_S;
    int64_t t0 = esp_timer_get_time();
    while (s_rec.written < total) {
        vTaskDelay(1);
        int64_t now_us = esp_timer_get_time() - t0;
        uint64_t due = (uint64_t)now_us * REC_SAMPLE_RATE_HZ / 1000000;
        if (due > total) {
            due = total;
        }
        while (s_rec.written < due) {
            size_t slot = s_rec.written % s_rec.capacity;
            make_sample(s_rec.written, &noise, s_rec.buf + slot * REC_SAMPLE_BYTES);
            s_rec.written++;
        }
        if (now_us >= (int64_t)next_report_s * 1000000 && next_report_s < seconds) {
            printf("PROGRESS {\"t_s\":%" PRIu32 ",\"written\":%" PRIu64 ",\"retained\":%" PRIu64 "}\n",
                   next_report_s, s_rec.written, retained());
            next_report_s += PROGRESS_EVERY_S;
        }
    }
    s_rec.elapsed_us = esp_timer_get_time() - t0;

    uint64_t kept = retained();
    uint64_t first = s_rec.written - kept;
    printf("\nRESULT {\"ok\":true,\"memory\":\"%s\",\"buffer_bytes\":%u,\"wanted_bytes\":%u,"
           "\"samples_written\":%" PRIu64 ",\"samples_retained\":%" PRIu64 ",\"first_sample\":%" PRIu64 ","
           "\"retained_s\":%.3f,\"elapsed_ms\":%" PRId64 ",\"internal_free_after\":%u}\n",
           s_rec.psram ? "psram" : "internal", (unsigned)bytes, (unsigned)want,
           s_rec.written, kept, first, (double)kept / REC_SAMPLE_RATE_HZ, s_rec.elapsed_us / 1000,
           (unsigned)heap_caps_get_free_size(INTERNAL_CAPS));
}

static size_t base64_encode(const uint8_t *in, size_t len, char *out)
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
    return o;
}

void rec_dump(void)
{
    if (s_rec.buf == NULL || s_rec.written == 0) {
        printf("\nDUMP_END {\"ok\":false,\"error\":\"no_recording\"}\n");
        return;
    }
    uint64_t kept = retained();
    uint64_t first = s_rec.written - kept;
    printf("\nDUMP_BEGIN {\"sample_rate\":%d,\"sample_bytes\":%d,\"format\":\"int16le_xyz\","
           "\"first_sample\":%" PRIu64 ",\"samples\":%" PRIu64 ",\"seconds\":%" PRIu32 "}\n",
           REC_SAMPLE_RATE_HZ, REC_SAMPLE_BYTES, first, kept, s_rec.seconds);

    static char line[(DUMP_CHUNK_SAMPLES * REC_SAMPLE_BYTES + 2) / 3 * 4 + 1];
    uint8_t chunk[DUMP_CHUNK_SAMPLES * REC_SAMPLE_BYTES];
    uint32_t crc = 0;
    uint32_t lines_out = 0;
    uint64_t n = first;
    while (n < s_rec.written) {
        size_t count = 0;
        while (count < DUMP_CHUNK_SAMPLES && n < s_rec.written) {
            size_t slot = n % s_rec.capacity;
            memcpy(chunk + count * REC_SAMPLE_BYTES, s_rec.buf + slot * REC_SAMPLE_BYTES, REC_SAMPLE_BYTES);
            count++;
            n++;
        }
        size_t len = count * REC_SAMPLE_BYTES;
        crc = esp_rom_crc32_le(crc, chunk, len);
        base64_encode(chunk, len, line);
        printf("D %s\n", line);
        if (++lines_out % DUMP_YIELD_EVERY == 0) {
            vTaskDelay(1);
        }
    }
    printf("DUMP_END {\"ok\":true,\"bytes\":%" PRIu64 ",\"crc32\":%" PRIu32 "}\n", kept * REC_SAMPLE_BYTES, crc);
    fflush(stdout);
}
