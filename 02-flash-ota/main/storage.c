#include "storage.h"

#include <stdbool.h>
#include "esp_vfs_fat.h"
#include "wear_levelling.h"

static wl_handle_t s_wl = WL_INVALID_HANDLE;

esp_err_t storage_mount(void)
{
    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    return esp_vfs_fat_spiflash_mount_rw_wl(STORAGE_BASE, STORAGE_LABEL, &cfg, &s_wl);
}

esp_err_t storage_format(void)
{
    return esp_vfs_fat_spiflash_format_rw_wl(STORAGE_BASE, STORAGE_LABEL);
}

void storage_usage(uint64_t *total, uint64_t *free_bytes)
{
    *total = 0;
    *free_bytes = 0;
    if (s_wl != WL_INVALID_HANDLE) {
        esp_vfs_fat_info(STORAGE_BASE, total, free_bytes);
    }
}
