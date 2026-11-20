# A1-01：8 MB PSRAM，能回看事故前数据的记录器

四块 XIAO（C3、C5、C6、S3）运行同一份固件，实时记录同一组**模拟**三轴振动数据，第 10 秒插入一段明显的异常尖峰。记录满 60 秒后，电脑再把每块板缓冲区里的数据取回来并排画图，看谁还能找回第 10 秒的异常。

## 1. 实验设计

| 项目 | 设定 |
| --- | --- |
| 数据 | 每秒 2000 组，每组 X/Y/Z 三个 int16，共 6 字节；X 为 50 Hz 振动，Y 为 120 Hz 振动，Z 为 1 g，均叠加少量噪声 |
| 异常 | 第 10.000 秒开始，持续 10 ms 的大幅值尖峰 |
| 完整历史 | 60 秒 × 2000 组 × 6 字节 = 720,000 字节 |
| 缓冲区 | 环形缓冲区，按完整 60 秒申请；内存放不下时按可用空间申请，写满后覆盖最早的数据 |
| 内存选择 | 有 PSRAM 的板（C5、S3）放 PSRAM；C3、C6 放内部 RAM，并给系统保留 32 KiB 空闲 |
| 导出 | 记录结束后才导出，电脑在记录期间只接收进度，不代存数据；导出数据带 CRC32 校验 |

板上只按样本序号生成数据，生成节奏与实时时钟对齐，四块板得到的数据内容一致，差别只来自能保留多长的历史。

## 2. 准备

| 项目 | 要求 |
| --- | --- |
| 硬件 | XIAO ESP32-C3、ESP32-C5、ESP32-C6、ESP32-S3（8 MB PSRAM 版本，例如 Sense 或 Plus），每块一根 USB-C 数据线 |
| 软件 | ESP-IDF **v6.1.1**（ESP32-C5 v1.0 芯片需要 v5.5.2 及以上） |
| 电脑端 | 使用 ESP-IDF 环境自带的 Python 与 pyserial，图表为脚本直接生成的 SVG |

## 3. 编译与烧录

每个芯片使用独立的构建目录和 sdkconfig，同一份源码可以同时保留四个目标的构建结果。构建目录放在纯英文路径下，因为工具链读取 specs 文件时需要 ASCII 路径。

```sh
get_idf
idf.py --version            # 应显示 ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  B="$HOME/.cache/a1-psram/$t"
  idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" build
done
```

用 `ls /dev/cu.usbmodem*` 找到各板串口，再烧录对应目标（**会替换该板现有固件**）：

```sh
B="$HOME/.cache/a1-psram/esp32c5"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

各目标的 Flash 大小、CPU 频率和 PSRAM 设置在 `sdkconfig.defaults.<芯片>` 中：C3/C6 为 4 MB Flash、160 MHz；C5/S3 为 8 MB Flash、240 MHz 并启用 PSRAM（S3 为 Octal 模式）。

## 4. 运行

每次测一块板，结果以板名记入同一个对比目录 `results/comparison/`，表格和图会自动包含已测过的所有板。只插一块板时可省略 `--port`：

```sh
python tools/run_psram.py --board C3 --note "USB powered, desk"
```

同时插多块板时，为每块板指定串口，可以在多个终端里并行运行：

```sh
python tools/run_psram.py --board C5 --port /dev/cu.usbmodem1201 --note "USB powered, desk"
python tools/run_psram.py --board C5-internal --port /dev/cu.usbmodem1201 --mem internal
```

每块板约 70 秒（60 秒记录 + 导出）。用同一个板名再测一次会替换该板的旧结果。

| 参数 | 作用 | 默认值 |
| --- | --- | --- |
| `--board 名称` | 该板在对比中的名称，例如 `C3`、`C5`、`C5-internal` | 必填 |
| `--port` | 串口 | 唯一的 `/dev/cu.usbmodem*` |
| `--mem auto\|internal` | `internal` 让有 PSRAM 的板只用内部 RAM，作为同一芯片的对照 | `auto` |
| `--seconds` | 记录时长，同一对比目录内所有板需一致 | `60` |
| `--note` | 该板测试条件的文字记录 | 空 |
| `--out` | 对比目录 | `results/comparison` |
| `--replot` | 只用已保存的数据重新生成表格和图 | — |

## 5. 结果文件

保存在对比目录 `results/comparison/`：

| 文件 | 内容 |
| --- | --- |
| `summary.md` | 每块板的缓冲区大小、保留时长与时间范围、异常是否仍在、导出耗时 |
| `waveforms.svg` | 所有板 X 轴历史并排在同一条 0–60 秒时间轴上，灰色区域为已被覆盖的部分 |
| `<名称>.bin` | 板上导出的原始样本，小端 int16 X/Y/Z 依次排列，按时间从早到晚 |
| `<名称>.json` | 板卡信息、内存使用、板端统计、测试条件 `note` 与记录时间 `recorded_at` |

异常是否保留由电脑端直接检查导出数据得出（|X| ≥ 15000 视为尖峰）。

## 6. 串口命令

也可以用任意串口终端直接操作：

| 命令 | 作用 |
| --- | --- |
| `info` | 输出芯片型号、版本、CPU 频率、PSRAM 与内部 RAM 空闲量 |
| `record seconds=60 mem=auto` | 实时记录；`mem=internal` 只用内部 RAM |
| `dump` | 以 `DUMP_BEGIN` / `D <base64>` / `DUMP_END` 行输出保留的样本 |

## 7. 结论口径

- 本实验比较的是**运行时缓存容量**：同一记录任务下，每块板能保留多长的原始历史。
- XIAO ESP32-S3 同样有 8 MB PSRAM，结果应与 C5 同属"完整保留"一组；对比重点是 C3、C6。
- 数据为模拟数据，结论不涉及 CPU 速度或传感器精度。
