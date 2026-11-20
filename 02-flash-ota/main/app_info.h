#pragma once

#include <stddef.h>

/*
 * Write a JSON object describing the board, firmware, flash layout, storage use and network into `buf`.
 * Returns the string length, or 0 when `buf` is too small.
 *
 * 将板卡、固件、Flash 分区、文件区使用量与网络状态以 JSON 对象写入 `buf`。
 * 返回字符串长度；`buf` 不足时返回 0。
 */
size_t app_info_json(char *buf, size_t len);
