#pragma once

#include <stdint.h>
#include "esp_err.h"

#define STORAGE_BASE  "/data"
#define STORAGE_LABEL "storage"

/* Mount the FAT storage partition at STORAGE_BASE, formatting it on first use.
 * 将 FAT 文件分区挂载到 STORAGE_BASE，首次使用时自动格式化。 */
esp_err_t storage_mount(void);

/* Erase every file on the storage partition.
 * 清空文件分区中的所有文件。 */
esp_err_t storage_format(void);

/* Filesystem capacity and free space in bytes; zeros when not mounted.
 * 文件系统总容量与剩余空间（字节）；未挂载时返回 0。 */
void storage_usage(uint64_t *total, uint64_t *free_bytes);
