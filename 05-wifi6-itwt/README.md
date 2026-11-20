# A1-05：Wi-Fi 6 省电，DTIM 省电与目标唤醒时间（TWT）

板子保持联网、做同一件轻量联网任务，只换省电方式，用 PPK2 测平均电流，同时测电脑给板子发消息的回复时间。

- C5、C6 支持 Wi-Fi 6，测 DTIM 省电和 TWT；C5 另测 5 GHz。
- C3、S3 是 Wi-Fi 4，只测 DTIM 省电，作为基线。

## 1. 实验设计

| 项目 | 设定 |
| --- | --- |
| 联网任务 | 保持连接；电脑每 3–7 秒（随机）发一次 PING，板子立即回 PONG；板子每 10 秒上报 64 字节 UDP |
| `ps` | 802.11 modem sleep，每个 DTIM 信标醒一次 |
| `twt<ms>` | 与路由器协商单独 TWT 协议，每 `<ms>` 毫秒醒一次；唤醒窗口 65 ms（同 ESP-IDF 示例） |
| `twt<ms>w<n>` | 同上，唤醒窗口改为 `<n>` 毫秒（1–65） |
| 芯片设置 | 所有阶段都开启自动浅睡和动态调频 |
| 供电 | PPK2 源模式 3.7 V 接电池焊盘（S3 用 4.2 V，满足其 Wi-Fi 启动约 730 mA 的峰值），USB 拔掉，100 kS/s 采样 |
| 统计 | 每阶段去掉开头 1 s、结尾 0.5 s 后求平均电流；另取阶段中间 3 s 波形；全部阶段结束后板子深睡，记录深睡电流 |
| 回复时间 | 电脑测 PING 往返时间，取中位数和 p90，并统计丢失数 |

默认每阶段 60 秒。默认计划：

| 芯片 | 计划 |
| --- | --- |
| C5 | `2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8,5g/ps,5g/twt500,5g/twt2000,5g/twt2000w8` |
| C6 | `2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8` |
| C3、S3 | `2g/ps` |

## 2. 准备

| 项目 | 要求 |
| --- | --- |
| 开发板 | 待测 XIAO 一块，USB-C 数据线一根 |
| 功耗分析仪 | Nordic PPK2，USB 线一根，接电脑 |
| 连线 | 两根细导线，焊到 XIAO 背面电池焊盘 |
| 路由器 | 开启 Wi-Fi 6（802.11ax）和 TWT；关闭 Smart Connect，2.4 GHz 与 5 GHz 分别命名。固件已打开 `CONFIG_ESP_WIFI_PASSIVE_HIDDEN_AP_SUPPORT`，可连接位于 DFS 信道上的隐藏网络 |
| 电脑 | 与板子在同一局域网（同一路由器下） |
| 软件 | ESP-IDF **v6.1.1**；电脑端使用本目录 `.venv`（含 ppk2-api、pyserial） |

在上一级目录的 `.env` 中填写网络（参考 `.env.example`）：

```
WIFI_SSID=2.4 GHz 网络名
WIFI_SSID_5G=5 GHz 网络名
WIFI_PASSWORD=密码
```

## 3. 接线

| PPK2 | XIAO |
| --- | --- |
| VOUT | 背面电池焊盘 `BAT+` |
| GND | 背面电池焊盘 `BAT-` |

PPK2 用自己的 USB 线接电脑。测量期间 XIAO 只由 PPK2 供电，USB 线拔掉；脚本会在打开 PPK2 输出之前提示拔线。

## 4. 编译与烧录

```sh
get_idf
idf.py --version            # 应显示 ESP-IDF v6.1.1

T=esp32c5                   # esp32c3 / esp32c5 / esp32c6 / esp32s3
B="$HOME/.cache/a1-wifi6/$T"
idf.py -B "$B/build" -DIDF_TARGET=$T -DSDKCONFIG="$B/sdkconfig" build
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

烧录会替换该板现有固件。

## 5. 运行

先插着 USB 试跑一遍，确认联网和消息通路（不需要 PPK2）：

```sh
.venv/bin/python tools/run_power.py --board C5 --dry-run --secs 20
```

正式测量：

```sh
.venv/bin/python tools/run_power.py --board C5 --note "RT-BE58 Go, 1 m"
```

1. 脚本通过 USB 把网络、电脑地址和计划写入板子，并设为“下次无 USB 上电时自动运行”。
2. 按提示拔掉 XIAO 的 USB 线；脚本检测到串口消失后，PPK2 开始供电和记录。
3. 板子联网后逐个阶段运行，终端实时显示每阶段的协商结果和平均电流。全部结束后板子深睡，脚本关闭 PPK2 输出并写入结果。

C5 默认计划每阶段 60 秒，全程约 10 分钟。用同一个板名再测一次会替换旧结果。

| 参数 | 作用 | 默认值 |
| --- | --- | --- |
| `--board 名称` | 本次结果在对比中的名称 | 必填 |
| `--port` | 板子串口 | 唯一的 `/dev/cu.usbmodem*` |
| `--ppk-port` | PPK2 串口 | 自动查找 |
| `--host` | 电脑在测试网络中的 IPv4 地址 | 默认路由所在网卡的地址 |
| `--plan` | 测试阶段 | 见第 1 节 |
| `--secs` | 每阶段秒数（10–600） | 60 |
| `--mv` | PPK2 输出电压（mV），S3 用 4200 | 3700 |
| `--dry-run` | 插 USB 试跑，不测电流 | 关 |
| `--note` | 测试条件备注 | 空 |
| `--replot` | 只根据已有结果重画报告 | 关 |

## 6. 结果

[`results/comparison/summary.md`](results/comparison/summary.md) 汇总所有板子的结果：每阶段的平均电流、相对 DTIM 省电的变化、PING 往返时间与丢失数。`current.svg` 是平均电流对比图，`traces.svg` 是各阶段 3 秒电流波形。
