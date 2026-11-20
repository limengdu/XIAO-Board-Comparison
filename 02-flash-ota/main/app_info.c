#include "app_info.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_idf_version.h"
#include "esp_image_format.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "net.h"
#include "sdkconfig.h"
#include "storage.h"

typedef struct {
    char *buf;
    size_t len;
    size_t pos;
    bool overflow;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    if (o->overflow) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->pos, o->len - o->pos, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= o->len - o->pos) {
        o->overflow = true;
    } else {
        o->pos += n;
    }
}

static uint32_t image_len(const esp_partition_t *p)
{
    const esp_partition_pos_t pos = { .offset = p->address, .size = p->size };
    esp_image_metadata_t meta;
    return esp_image_get_metadata(&pos, &meta) == ESP_OK ? meta.image_len : 0;
}

size_t app_info_json(char *buf, size_t len)
{
    out_t o = { .buf = buf, .len = len };
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash_bytes = 0;
    esp_flash_get_size(NULL, &flash_bytes);
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *boot = esp_ota_get_boot_partition();
    uint64_t fs_total, fs_free;
    storage_usage(&fs_total, &fs_free);
    char ip[16];
    net_ip(ip, sizeof(ip));

    put(&o, "{\"target\":\"%s\",\"revision\":%u,\"version\":\"%s\",\"idf\":\"%s\",\"flash_bytes\":%" PRIu32 ","
            "\"running\":\"%s\",\"boot\":\"%s\",\"app_bytes\":%" PRIu32 ","
            "\"fs_total\":%" PRIu64 ",\"fs_free\":%" PRIu64 ",\"ip\":\"%s\",\"rssi\":%d,\"partitions\":[",
        CONFIG_IDF_TARGET, chip.revision, esp_app_get_description()->version, esp_get_idf_version(), flash_bytes,
        running->label, boot ? boot->label : "", image_len(running), fs_total, fs_free, ip, net_rssi());

    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_ANY, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (bool first = true; it != NULL; it = esp_partition_next(it), first = false) {
        const esp_partition_t *p = esp_partition_get(it);
        put(&o, "%s{\"label\":\"%s\",\"type\":%d,\"subtype\":%d,\"offset\":%" PRIu32 ",\"size\":%" PRIu32 "}",
            first ? "" : ",", p->label, p->type, p->subtype, p->address, p->size);
    }
    esp_partition_iterator_release(it);
    put(&o, "]}");
    return o.overflow ? 0 : o.pos;
}
