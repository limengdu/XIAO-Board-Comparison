#include "bus_test.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#define ID_BASE         0x0A00000u      /* extended ID of frame 0; frame n uses ID_BASE + n / 第 0 帧的扩展 ID，第 n 帧为 ID_BASE + n */
#define TX_QUEUE_DEPTH  16
#define TX_SLOTS        (TX_QUEUE_DEPTH + 8)
#define SLOT_WAIT_MS    500             /* longest wait for a free TX slot / 等待空闲发送槽的最长时间 */
#define DRAIN_WAIT_MS   1000            /* longest wait for the last frames after the final transmit / 最后一次发送后等待剩余帧的最长时间 */
#define MAX_FRAMES      (BUS_MAX_BYTES / 8)
#define PATTERN_SEED    0x2545F491u

typedef struct {
    /* receiver side / 接收端 */
    uint8_t *rx_buf;
    uint32_t frames;
    uint32_t payload;
    volatile uint32_t rx_frames;
    volatile uint32_t rx_unique;
    volatile uint32_t dup;
    volatile uint32_t out_of_order;
    volatile uint32_t bad_frame;
    volatile uint32_t next_seq;
    volatile int64_t first_rx_us;
    volatile int64_t last_rx_us;
    SemaphoreHandle_t rx_all;
    /* sender side / 发送端 */
    SemaphoreHandle_t slots;
    volatile uint32_t tx_ok;
    volatile uint32_t tx_fail;
    /* bus errors seen by each node / 每个节点观察到的总线错误 */
    volatile uint32_t err_a, err_b;
    volatile uint32_t err_flags_a, err_flags_b;
} test_state_t;

static test_state_t s_st;
static uint8_t s_seen[MAX_FRAMES / 8];
static twai_frame_t s_slots[TX_SLOTS];

static const char *mode_name(bus_mode_t mode)
{
    switch (mode) {
    case BUS_CLASSIC:
        return "classic";
    case BUS_FD_NOBRS:
        return "fd_nobrs";
    default:
        return "fd";
    }
}

/* Fill `buf` with a fixed pseudo-random pattern (xorshift32), identical on every run.
 * 用固定的伪随机序列（xorshift32）填充 `buf`，每次运行都相同。 */
static void fill_pattern(uint8_t *buf, uint32_t len)
{
    uint32_t x = PATTERN_SEED;
    for (uint32_t i = 0; i < len; i++) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (uint8_t)x;
    }
}

/* Receiver: copy each frame to its place in the data block by sequence number.
 * 接收端：按序号把每帧数据放到数据块中对应的位置。 */
static bool on_rx_b(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *ctx)
{
    test_state_t *st = ctx;
    uint8_t data[TWAIFD_FRAME_MAX_LEN];
    twai_frame_t frame = { .buffer = data, .buffer_len = sizeof(data) };
    if (twai_node_receive_from_isr(node, &frame) != ESP_OK) {
        return false;
    }
    int64_t now = esp_timer_get_time();
    st->rx_frames++;
    uint32_t seq = frame.header.id - ID_BASE;
    uint32_t len = twaifd_dlc2len(frame.header.dlc);
    if (!frame.header.ide || seq >= st->frames || len != st->payload) {
        st->bad_frame++;
        return false;
    }
    if (st->rx_unique == 0) {
        st->first_rx_us = now;
    }
    st->last_rx_us = now;
    if (seq != st->next_seq) {
        st->out_of_order++;
    }
    st->next_seq = seq + 1;
    if (s_seen[seq / 8] & (1u << (seq % 8))) {
        st->dup++;
        return false;
    }
    s_seen[seq / 8] |= 1u << (seq % 8);
    memcpy(st->rx_buf + seq * st->payload, data, len);
    BaseType_t woken = pdFALSE;
    if (++st->rx_unique == st->frames) {
        xSemaphoreGiveFromISR(st->rx_all, &woken);
    }
    return woken == pdTRUE;
}

/* Sender: each finished frame frees one TX slot.
 * 发送端：每发完一帧释放一个发送槽。 */
static bool on_tx_done_a(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *ctx)
{
    test_state_t *st = ctx;
    if (edata->is_tx_success) {
        st->tx_ok++;
    } else {
        st->tx_fail++;
    }
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(st->slots, &woken);
    return woken == pdTRUE;
}

static bool on_error_a(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    test_state_t *st = ctx;
    st->err_a++;
    st->err_flags_a |= edata->err_flags.val;
    return false;
}

static bool on_error_b(twai_node_handle_t node, const twai_error_event_data_t *edata, void *ctx)
{
    test_state_t *st = ctx;
    st->err_b++;
    st->err_flags_b |= edata->err_flags.val;
    return false;
}

/* Create and start one node on the given pins with the run's bit timing.
 * 用本轮的位时序在指定引脚上创建并启动一个节点。 */
static esp_err_t make_node(const bus_run_cfg_t *cfg, int tx, int rx, const twai_event_callbacks_t *cbs,
                           twai_node_handle_t *out)
{
    twai_onchip_node_config_t nc = {
        .io_cfg = {
            .tx = tx,
            .rx = rx,
            .quanta_clk_out = GPIO_NUM_NC,
            .bus_off_indicator = GPIO_NUM_NC,
        },
        .bit_timing = { .bitrate = cfg->arb_bps },
        .data_timing = { .bitrate = cfg->mode == BUS_CLASSIC ? 0 : cfg->data_bps },
        .fail_retry_cnt = -1,
        .tx_queue_depth = TX_QUEUE_DEPTH,
    };
    esp_err_t err = twai_new_node_onchip(&nc, out);
    if (err != ESP_OK) {
        return err;
    }
    err = twai_node_register_event_callbacks(*out, cbs, &s_st);
    if (err == ESP_OK) {
        err = twai_node_enable(*out);
    }
    if (err != ESP_OK) {
        twai_node_delete(*out);
        *out = NULL;
    }
    return err;
}

static void free_node(twai_node_handle_t node)
{
    if (node != NULL) {
        twai_node_disable(node);
        twai_node_delete(node);
    }
}

static void print_fail(const bus_run_cfg_t *cfg, const char *error, esp_err_t err)
{
    printf("\nRESULT {\"ok\":false,\"mode\":\"%s\",\"run\":%" PRIu32 ",\"error\":\"%s\",\"esp_err\":\"%s\"}\n",
           mode_name(cfg->mode), cfg->run, error, esp_err_to_name(err));
}

void bus_print_info(void)
{
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    printf("\nINFO {\"target\":\"%s\",\"revision\":%u,\"cpu_mhz\":%d,\"idf\":\"%s\",\"controllers\":%d,"
           "\"fd_supported\":%s,\"node_a\":{\"tx\":%d,\"rx\":%d},\"node_b\":{\"tx\":%d,\"rx\":%d},"
           "\"max_bytes\":%d}\n",
           CONFIG_IDF_TARGET, chip.revision, CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ, esp_get_idf_version(),
           (int)SOC_TWAI_CONTROLLER_NUM,
#if SOC_TWAI_FD_SUPPORTED
           "true",
#else
           "false",
#endif
           BUS_TX_GPIO_A, BUS_RX_GPIO_A, BUS_TX_GPIO_B, BUS_RX_GPIO_B, BUS_MAX_BYTES);
}

/*
 * Send one classic frame from each node with the other node switched off, and print the error flags each saw.
 * Only ack_err means the node's transceiver echoes the bus correctly; bit or form errors mean its TX, RX or power path is open.
 *
 * 在另一个节点关闭时，让每个节点各发一帧经典帧，并输出各自看到的错误标志。
 * 只有 ack_err 表示该节点的收发器能正确回读总线；出现位错误或格式错误表示其 TX、RX 或供电线路不通。
 */
void bus_probe(void)
{
    const bus_run_cfg_t cfg = { .mode = BUS_CLASSIC, .arb_bps = 500000 };
    const int pins[2][2] = { { BUS_TX_GPIO_A, BUS_RX_GPIO_A }, { BUS_TX_GPIO_B, BUS_RX_GPIO_B } };
    const twai_event_callbacks_t cbs = { .on_error = on_error_a };
    uint32_t flags[2] = {0};
    esp_err_t errs[2] = {ESP_OK, ESP_OK};
    static uint8_t data[8] = { 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa, 0x55, 0xaa };
    for (int i = 0; i < 2; i++) {
        twai_node_handle_t node = NULL;
        s_st.err_a = 0;
        s_st.err_flags_a = 0;
        errs[i] = make_node(&cfg, pins[i][0], pins[i][1], &cbs, &node);
        if (errs[i] == ESP_OK) {
            twai_frame_t f = { .header = { .id = ID_BASE, .ide = 1 }, .buffer = data, .buffer_len = sizeof(data) };
            errs[i] = twai_node_transmit(node, &f, 100);
            vTaskDelay(pdMS_TO_TICKS(50));
            free_node(node);
        }
        flags[i] = s_st.err_flags_a;
    }
    printf("\nPROBE {\"node_a\":{\"err_flags\":%" PRIu32 ",\"esp_err\":\"%s\"},"
           "\"node_b\":{\"err_flags\":%" PRIu32 ",\"esp_err\":\"%s\"}}\n",
           flags[0], esp_err_to_name(errs[0]), flags[1], esp_err_to_name(errs[1]));
}

void bus_run(const bus_run_cfg_t *cfg)
{
    const bool fd = cfg->mode != BUS_CLASSIC;
    const uint32_t payload = fd ? 64 : 8;
    if (cfg->bytes == 0 || cfg->bytes % payload != 0 || cfg->bytes > BUS_MAX_BYTES) {
        print_fail(cfg, "bad_size", ESP_ERR_INVALID_ARG);
        return;
    }
    uint8_t *tx_buf = heap_caps_malloc(cfg->bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *rx_buf = heap_caps_calloc(1, cfg->bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (s_st.slots == NULL) {
        s_st.slots = xSemaphoreCreateCounting(TX_SLOTS, 0);
        s_st.rx_all = xSemaphoreCreateBinary();
    }
    if (tx_buf == NULL || rx_buf == NULL || s_st.slots == NULL || s_st.rx_all == NULL) {
        free(tx_buf);
        free(rx_buf);
        print_fail(cfg, "no_mem", ESP_ERR_NO_MEM);
        return;
    }
    fill_pattern(tx_buf, cfg->bytes);

    SemaphoreHandle_t slots = s_st.slots;
    SemaphoreHandle_t rx_all = s_st.rx_all;
    memset(&s_st, 0, sizeof(s_st));
    memset(s_seen, 0, sizeof(s_seen));
    s_st.slots = slots;
    s_st.rx_all = rx_all;
    s_st.rx_buf = rx_buf;
    s_st.payload = payload;
    s_st.frames = cfg->bytes / payload;
    while (xSemaphoreTake(slots, 0) == pdTRUE) {
    }
    for (int i = 0; i < TX_SLOTS; i++) {
        xSemaphoreGive(slots);
    }
    xSemaphoreTake(rx_all, 0);

    const twai_event_callbacks_t cbs_a = { .on_tx_done = on_tx_done_a, .on_error = on_error_a };
    const twai_event_callbacks_t cbs_b = { .on_rx_done = on_rx_b, .on_error = on_error_b };
    twai_node_handle_t node_a = NULL, node_b = NULL;
    esp_err_t err = make_node(cfg, BUS_TX_GPIO_B, BUS_RX_GPIO_B, &cbs_b, &node_b);
    if (err == ESP_OK) {
        err = make_node(cfg, BUS_TX_GPIO_A, BUS_RX_GPIO_A, &cbs_a, &node_a);
    }
    if (err != ESP_OK) {
        free_node(node_b);
        free(tx_buf);
        free(rx_buf);
        print_fail(cfg, "node_init", err);
        return;
    }

    const char *error = NULL;
    uint32_t sent = 0;
    int64_t t0 = esp_timer_get_time();
    for (; sent < s_st.frames; sent++) {
        if (xSemaphoreTake(slots, pdMS_TO_TICKS(SLOT_WAIT_MS)) != pdTRUE) {
            error = "tx_stalled";
            break;
        }
        twai_frame_t *f = &s_slots[sent % TX_SLOTS];
        *f = (twai_frame_t){
            .header = { .id = ID_BASE + sent, .ide = 1, .fdf = fd, .brs = cfg->mode == BUS_FD },
            .buffer = tx_buf + sent * payload,
            .buffer_len = payload,
        };
        err = twai_node_transmit(node_a, f, SLOT_WAIT_MS);
        if (err != ESP_OK) {
            error = "tx_queue";
            break;
        }
    }
    int64_t t_queued = esp_timer_get_time();
    if (error == NULL && xSemaphoreTake(rx_all, pdMS_TO_TICKS(DRAIN_WAIT_MS)) != pdTRUE) {
        error = "rx_incomplete";
    }

    twai_node_status_t st_a = {0}, st_b = {0};
    twai_node_record_t rec_a = {0}, rec_b = {0};
    twai_node_get_info(node_a, &st_a, &rec_a);
    twai_node_get_info(node_b, &st_b, &rec_b);
    free_node(node_a);
    free_node(node_b);

    uint32_t crc_tx = esp_rom_crc32_le(0, tx_buf, cfg->bytes);
    uint32_t crc_rx = esp_rom_crc32_le(0, rx_buf, cfg->bytes);
    bool match = crc_tx == crc_rx && s_st.rx_unique == s_st.frames;
    int32_t first_diff = -1;
    uint32_t diff_bytes = 0;
    for (uint32_t i = 0; i < cfg->bytes; i++) {
        if (tx_buf[i] != rx_buf[i]) {
            if (first_diff < 0) {
                first_diff = (int32_t)i;
            }
            diff_bytes++;
        }
    }
    int64_t elapsed = s_st.rx_unique ? s_st.last_rx_us - t0 : 0;

    printf("\nRESULT {\"ok\":%s,\"mode\":\"%s\",\"run\":%" PRIu32 ",\"error\":%s%s%s,"
           "\"arb_bps\":%" PRIu32 ",\"data_bps\":%" PRIu32 ",\"brs\":%s,\"payload\":%" PRIu32 ","
           "\"bytes\":%" PRIu32 ",\"frames\":%" PRIu32 ",\"sent\":%" PRIu32 ",\"tx_ok\":%" PRIu32 ","
           "\"tx_fail\":%" PRIu32 ",\"rx_frames\":%" PRIu32 ",\"rx_unique\":%" PRIu32 ",\"missing\":%" PRIu32 ","
           "\"dup\":%" PRIu32 ",\"out_of_order\":%" PRIu32 ",\"bad_frame\":%" PRIu32 ","
           "\"crc_tx\":%" PRIu32 ",\"crc_rx\":%" PRIu32 ",\"match\":%s,"
           "\"first_diff\":%" PRId32 ",\"diff_bytes\":%" PRIu32 ","
           "\"elapsed_us\":%" PRId64 ",\"queue_us\":%" PRId64 ",\"first_rx_us\":%" PRId64 ","
           "\"node_a\":{\"errors\":%" PRIu32 ",\"err_flags\":%" PRIu32 ",\"bus_err_num\":%" PRIu32 ",\"tec\":%u,\"rec\":%u,\"state\":%d},"
           "\"node_b\":{\"errors\":%" PRIu32 ",\"err_flags\":%" PRIu32 ",\"bus_err_num\":%" PRIu32 ",\"tec\":%u,\"rec\":%u,\"state\":%d}}\n",
           error == NULL && match ? "true" : "false", mode_name(cfg->mode), cfg->run,
           error ? "\"" : "", error ? error : "null", error ? "\"" : "",
           cfg->arb_bps, fd ? cfg->data_bps : cfg->arb_bps, cfg->mode == BUS_FD ? "true" : "false", payload,
           cfg->bytes, s_st.frames, sent, s_st.tx_ok, s_st.tx_fail, s_st.rx_frames, s_st.rx_unique,
           s_st.frames - s_st.rx_unique, s_st.dup, s_st.out_of_order, s_st.bad_frame,
           crc_tx, crc_rx, match ? "true" : "false", first_diff, diff_bytes,
           elapsed, t_queued - t0, s_st.rx_unique ? s_st.first_rx_us - t0 : 0,
           s_st.err_a, s_st.err_flags_a, rec_a.bus_err_num, st_a.tx_error_count, st_a.rx_error_count, (int)st_a.state,
           s_st.err_b, s_st.err_flags_b, rec_b.bus_err_num, st_b.tx_error_count, st_b.rx_error_count, (int)st_b.state);
    free(tx_buf);
    free(rx_buf);
}
