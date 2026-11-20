# A1-03：240 MHz CPU，渲染同一组四张图要多久

四块 XIAO（C3、C5、C6、S3）用同一份固件渲染同一组四张分形图，在板上计时，比较完成时间。每块板算出的图逐像素一致，差别只在花了多长时间。

## 1. 实验设计

| 项目 | 设定 |
| --- | --- |
| 图像 | 四张分形图：Mandelbrot 全图、海马谷、象谷、Julia 集；每张 320×240，每像素最多 255 次迭代，四张共 26,230,366 次迭代 |
| 运算 | 只用整数定点运算（Q7.24），各芯片的计算结果完全相同，也不依赖浮点单元 |
| 两种方式 | `seq`：一个任务依次渲染四张图。`par`：四个任务同时各渲染一张；双核的 S3 会把任务分到两个核上 |
| 计时 | 板上用 `esp_timer` 只计算渲染过程；Wi-Fi 关闭，串口在计时期间不输出 |
| 统一条件 | 同一份源码、ESP-IDF 版本、`-O2` 优化；各板使用正常最高频率：C3/C6 160 MHz，C5/S3 240 MHz |
| 重复 | 每种方式默认 5 次，取中位数，并记录最小值和最大值 |
| 校验 | 每张图计时时计算 CRC32；之后不计时地重新渲染并传给电脑，确认与计时那次一致，并与其他板比对 |

工作缓冲只有一行像素（320 字节），放在内部 RAM，内存大小不影响结果。

## 2. 准备

| 项目 | 要求 |
| --- | --- |
| 硬件 | XIAO ESP32-C3、ESP32-C5、ESP32-C6、ESP32-S3，每块一根 USB-C 数据线 |
| 软件 | ESP-IDF **v6.1.1** |
| 电脑端 | ESP-IDF 环境自带的 Python 与 pyserial；图片和图表由脚本直接生成 |

## 3. 编译与烧录

```sh
get_idf
idf.py --version            # 应显示 ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  B="$HOME/.cache/a1-cpu/$t"
  idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" build
done

B="$HOME/.cache/a1-cpu/esp32c5"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

烧录会替换该板现有固件。各芯片的 CPU 频率在 `sdkconfig.defaults.<芯片>` 中设置。渲染期间 CPU 持续满载，所以 `sdkconfig.defaults` 关闭了任务看门狗。

## 4. 运行

每次测一块板，结果以板名记入 `results/comparison/`：

```sh
python tools/run_cpu.py --board C3 --note "USB powered, desk"
python tools/run_cpu.py --board S3 --port /dev/cu.usbmodem2101
```

每块板约 1–2 分钟。用同一个板名再测一次会替换该板的旧结果。

| 参数 | 作用 | 默认值 |
| --- | --- | --- |
| `--board 名称` | 该板在对比中的名称 | 必填 |
| `--port` | 串口 | 唯一的 `/dev/cu.usbmodem*` |
| `--runs` | 每种方式的重复次数 | `5` |
| `--note` | 该板测试条件的文字记录 | 空 |
| `--out` | 对比目录 | `results/comparison` |
| `--replot` | 只用已保存的数据重新生成表格和图 | — |

## 5. 结果文件

| 文件 | 内容 |
| --- | --- |
| `summary.md` | 每块板两种方式的中位数、最小与最大耗时、相对 C3 的速度、像素是否与其他板一致 |
| `times.svg` | 所有板两种方式的耗时条形图 |
| `<名称>.png` | 该板算出的四张图，拼成 2×2 |
| `<名称>.json` | 板卡信息、每次运行的总耗时与每张图的耗时、CRC32、迭代次数 |

## 6. 串口命令

| 命令 | 作用 |
| --- | --- |
| `info` | 芯片、核心数、CPU 频率、图像尺寸 |
| `bench mode=seq runs=5` | 计时渲染；`mode=par` 为四个任务同时渲染 |
| `dump <0-3>` | 不计时渲染一张图，以 `IMG_BEGIN` / `R <base64>` / `IMG_END` 行输出像素 |

## 7. 结论口径

- 结果只代表这类整数计算任务，不推导整体性能排名。
- `seq` 比较的是单个核心的速度。C5 与 C3、C6 的差距同时来自主频和每个时钟周期的效率，不能只按 240/160 推算。
- `par` 中单核芯片的四个任务轮流使用同一个核心，总时间与 `seq` 相同；S3 的两个核心同时工作，因此更快。四张图计算量不同，后半段只剩最重的一张在算，所以 S3 的提升小于两倍。
