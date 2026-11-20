# A1-04：CAN FD，经典 CAN 与 CAN FD 传输同一数据块

一块 XIAO ESP32-C5 用自带的两路 TWAI 控制器，各接一块 CAN 收发器，组成一条真实的两节点总线。节点 A 把同一个数据块发给节点 B，分别用经典 CAN 和 CAN FD 传输，比较完成时间，并逐字节核对收到的数据。

C3、C6、S3 的控制器只支持经典 CAN，在对比表中标为“不支持 CAN FD”。

## 1. 实验设计

| 项目 | 设定 |
| --- | --- |
| 数据块 | 64 KiB 固定伪随机数据，每次运行内容相同 |
| 三种方式 | `classic`：经典 CAN，每帧 8 字节。`fd_nobrs`：CAN FD，每帧 64 字节，整帧都用仲裁速率。`fd`：CAN FD，每帧 64 字节，数据段切换到更高的数据速率（BRS） |
| 速率 | 仲裁段 500 kbps；FD 数据段 2 Mbps |
| 帧格式 | 29 位扩展 ID，ID 中携带帧序号，数据区全部用于传数据 |
| 计时 | 板上 `esp_timer`，从第一帧开始发送到最后一帧收到 |
| 校验 | 接收端按序号把每帧放回原位，统计丢帧、重复帧和乱序帧，最后比对两份数据的 CRC32 |
| 错误统计 | 两个节点各自的总线错误次数、发送/接收错误计数和错误状态 |
| 重复 | 每种方式默认 3 次，取中位数 |

`fd_nobrs` 只改变每帧的数据量，`fd` 在此基础上再提高数据段速率，两者分开就能看出各自贡献了多少。

## 2. 准备

| 项目 | 要求 |
| --- | --- |
| 开发板 | XIAO ESP32-C5 一块，USB-C 数据线一根 |
| 收发器 | Adafruit CAN Pal（5708，TJA1051T/3）两块，排针已焊好 |
| 连线 | 杜邦线 8 根；双绞线一对（CANH、CANL），长度 1 m 以内 |
| 软件 | ESP-IDF **v6.1.1**；电脑端使用 ESP-IDF 环境自带的 Python 与 pyserial |

## 3. 接线

接线前先拔掉 XIAO 的 USB 线，全部接好再上电。

| CAN Pal A（发送节点） | XIAO ESP32-C5 |
| --- | --- |
| Vcc | 3V3 |
| GND | GND |
| TX | D4（GPIO23） |
| RX | D5（GPIO24） |
| SLNT | 悬空 |

| CAN Pal B（接收节点） | XIAO ESP32-C5 |
| --- | --- |
| Vcc | 3V3 |
| GND | GND |
| TX | D8（GPIO8） |
| RX | D9（GPIO9） |
| SLNT | 悬空 |

两块 CAN Pal 之间：A 的 `H` 接 B 的 `H`，A 的 `L` 接 B 的 `L`，用双绞线。两块板的 Termination 开关都拨到 **ON**，总线两端各一个 120 Ω。

CAN Pal 的 TX 是控制器的输出，直接接 XIAO 上的 TX 引脚，RX 同理。CAN Pal 上的排针 `CANH`/`CANL` 与端子 `H`/`L` 是同一组信号，任选一处连接两块板；端子中间一位是 GND。

## 4. 编译与烧录

```sh
get_idf
idf.py --version            # 应显示 ESP-IDF v6.1.1

B="$HOME/.cache/a1-canfd/esp32c5"
idf.py -B "$B/build" -DIDF_TARGET=esp32c5 -DSDKCONFIG="$B/sdkconfig" build
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

烧录会替换该板现有固件。

## 5. 运行

```sh
python tools/run_canfd.py --board C5 --note "two CAN Pal, 0.3 m twisted pair"
python tools/run_canfd.py --board C5-5M --data 5000000      # 另测 5 Mbps 数据段
```

三种方式各跑 3 次，约 30 秒。用同一个板名再测一次会替换旧结果。节点 A 发出的帧一直没有被确认时，脚本会提示检查供电、引脚、CANH/CANL 与终端电阻。

| 参数 | 作用 | 默认值 |
| --- | --- | --- |
| `--board 名称` | 本次结果在对比中的名称 | 必填 |
| `--port` | 串口 | 唯一的 `/dev/cu.usbmodem*` |
| `--bytes` | 数据块大小，64 的倍数，最大 131072 | `65536` |
| `--arb` | 仲裁段速率 | `500000` |
| `--data` | FD 数据段速率 | `2000000` |
| `--runs` | 每种方式的重复次数 | `3` |
| `--note` | 测试条件的文字记录 | 空 |
| `--out` | 对比目录 | `results/comparison` |
| `--replot` | 只用已保存的数据重新生成表格和图 | — |

## 6. 结果文件

| 文件 | 内容 |
| --- | --- |
| `summary.md` | 每种方式的帧数、中位耗时、有效数据速率、相对经典 CAN 的倍数、数据是否完整、总线错误；以及各芯片的控制器能力 |
| `times.svg` | 各方式耗时条形图 |
| `<名称>.json` | 板卡信息、测试参数、每次运行的完整板上结果 |

## 7. 串口命令

| 命令 | 作用 |
| --- | --- |
| `info` | 芯片、控制器数量、是否支持 FD、两个节点的引脚 |
| `probe` | 关闭一个节点，让另一个节点单独发一帧，输出两个节点各自的错误标志。`err_flags` 为 16（只有无应答）表示该节点的 CAN Pal 接线正确；含 2 或 4（位错误、格式错误）表示该节点的 Vcc、GND、TX 或 RX 有一根没接通 |
| `run mode=fd bytes=65536 arb=500000 data=2000000` | 传输一次数据块并输出一行 `RESULT {...}`；`mode` 可选 `classic`、`fd_nobrs`、`fd` |

## 8. 结论口径

- 64 字节帧的数据量是 8 字节帧的 8 倍，但每帧的 ID、控制位、CRC 和帧间隔开销仍然存在，实际提速以 *vs classic* 一列的实测倍数为准。
- 两个节点在同一块芯片上，结果反映的是控制器和收发器在真实总线上的表现；与其他品牌设备互通需另行测试。
- C3、C6、S3 只比较控制器能力，不给出 CAN FD 数值。
