#pragma once

#include "esp_err.h"

/*
 * Start the HTTP server:
 *   GET  /api/info           board, flash layout and storage use (JSON)
 *   POST /api/format         erase every stored file
 *   PUT  /api/files/<path>   store a file; 507 when storage is full
 *   GET  /api/files/<path>   read a stored file byte for byte
 *   DELETE /api/files/<path> remove a stored file
 *   POST /api/ota           write a firmware image to the idle OTA slot and reboot into it
 *   GET  /<path>             serve <path> or <path>.gz from the www/ folder of the storage partition
 *
 * 启动 HTTP 服务：
 *   GET  /api/info           板卡、Flash 分区与文件区使用量（JSON）
 *   POST /api/format         清空所有文件
 *   PUT  /api/files/<path>   写入文件；空间不足时返回 507
 *   GET  /api/files/<path>   原样读取已存文件
 *   DELETE /api/files/<path> 删除已存文件
 *   POST /api/ota            把固件写入空闲 OTA 分区并重启进入新固件
 *   GET  /<path>             从文件区 www/ 目录提供 <path> 或 <path>.gz
 */
esp_err_t web_start(void);
