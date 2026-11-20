# A1：XIAO ESP32-C3 / C5 / C6 / S3 对比测评

对比文章《已有 C3、C6 或 S3，还需要 XIAO ESP32-C5 吗？》的实验工程。每个部分是一个独立工程，四块板运行同一份源码，用同一个实际任务比较结果。

| 部分 | 目录 | 用户任务 | 状态 |
| --- | --- | --- | --- |
| 1. 8 MB PSRAM | [01-psram-recorder](01-psram-recorder/README.md) | 记录器能否回看事故发生前的数据 | 四板实测完成，[对比结果](01-psram-recorder/results/comparison/summary.md) |
| 2. 8 MB Flash | [02-flash-ota](02-flash-ota/README.md) | 保留 OTA 升级能力后还能放多少内容 | 四板实测完成，[对比结果](02-flash-ota/results/comparison/summary.md) |
| 3. 240 MHz CPU | [03-cpu-render](03-cpu-render/README.md) | 渲染同一组四张图所需的时间 | 四板实测完成，[对比结果](03-cpu-render/results/comparison/summary.md) |
| 4. CAN FD | [04-canfd](04-canfd/README.md) | 经典 CAN 与 CAN FD 传输同一数据块 | C5 双节点真实总线实测完成，[对比结果](04-canfd/results/comparison/summary.md) |
| 5. Wi-Fi 6 省电 | `05-wifi6-itwt` | 开启 iTWT 后相同联网任务的耗电与响应 | 待确认 AP 支持 iTWT 与功耗仪 |

## 通用约定

- ESP-IDF **v6.1.1**，四个目标均在同一版本下编译。
- 需要联网的实验从本目录的 `.env` 读取 `WIFI_SSID` 与 `WIFI_PASSWORD`，格式见 `.env.example`。
- 每个工程用 `idf.py -B "$HOME/.cache/<工程>/<芯片>/build" -DIDF_TARGET=<芯片> -DSDKCONFIG=...` 为每个芯片单独构建，构建目录放在纯英文路径下。
- 测试对象是具体的 XIAO 板卡，结果包含板级供电、天线和 PCB 等因素。
- 每次测一块板，原始数据、板卡信息和测试条件按板名存入工程的 `results/comparison/`，对比表和图随之更新。
