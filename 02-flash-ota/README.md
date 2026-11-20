# A1-02：8 MB Flash，保留 OTA 后还能放多少内容

四块 XIAO（C3、C5、C6、S3）运行同一份网页仪表盘固件，使用相同的分区规则：两个 1.5 MB 的 OTA 固件分区，Flash 其余空间作为 FAT 文件区。电脑按固定顺序往文件区装入同一批真实的开源网页资源，看每块板能装下多少；再通过网络把固件从 V1 升级到 V2，确认升级后文件和网页都完好。

## 1. 实验设计

| 项目 | 设定 |
| --- | --- |
| 固件 | Wi-Fi 网页服务 + HTTP OTA；V1 与 V2 来自同一份源码，只有版本号不同 |
| 分区 | `ota_0`、`ota_1` 各 1.5 MB；其余为 `storage` 文件区（4 MB 板约 0.9 MB，8 MB 板约 4.9 MB） |
| 资源包 | 按顺序安装，见下表；文本类文件 gzip 压缩后存储，字体文件原样存储 |
| 放不下时 | 当前资源包整体删除，后续资源包跳过，保证板上只有完整的资源包 |
| 升级 | 电脑把 V2 固件 POST 到 `/api/ota`，板子写入空闲分区后重启进入 V2 |
| 校验 | 升级后逐个读回所有已存文件比对 SHA-256，并请求仪表盘首页 |

| 顺序 | 资源包 | 内容 | 存储大小 |
| --- | --- | --- | ---: |
| 1 | `dashboard` | 仪表盘页面 + Chart.js 4.4.1 | 约 71 KiB |
| 2 | `icons` | Font Awesome Free 6.5.1 实心与常规图标 | 约 202 KiB |
| 3 | `latin-font` | Inter 英文字体，常规与粗体 | 约 46 KiB |
| 4 | `cjk-regular` | Noto Sans SC 中文字体，常规 | 约 3.4 MiB |
| 5 | `cjk-bold` | Noto Sans SC 中文字体，粗体 | 约 3.5 MiB |

资源文件来自 npm 上锁定版本的开源包，下载时按 jsDelivr 提供的 SHA-256 校验。各资源的许可证文件随资源包一同存到板上的 `www/licenses/`。

## 2. 准备

| 项目 | 要求 |
| --- | --- |
| 硬件 | XIAO ESP32-C3、ESP32-C5、ESP32-C6、ESP32-S3，每块一根 USB-C 数据线 |
| 网络 | 一个 2.4 GHz Wi-Fi，电脑在同一局域网；C5 也可以连 5 GHz |
| 软件 | ESP-IDF **v6.1.1** |
| Wi-Fi 配置 | 在 `A1-XIAO-Board-Comparison/.env` 中填写 `WIFI_SSID` 和 `WIFI_PASSWORD`，格式见 `.env.example` |

## 3. 下载资源

```sh
python3 tools/fetch_assets.py
```

文件保存在 `assets/cache/`，重复运行时跳过已下载且校验通过的文件。

## 4. 编译与烧录

每个芯片的 V1、V2 各用一个构建目录：

```sh
get_idf
idf.py --version            # 应显示 ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  for v in 1.0.0 2.0.0; do
    B="$HOME/.cache/a1-flash/$t/v$v"
    idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" -DPROJECT_VER=$v build
  done
done
```

用 USB 烧录 V1（**会替换该板现有固件**）；V2 由测试脚本通过网络升级：

```sh
B="$HOME/.cache/a1-flash/esp32c5/v1.0.0"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

4 MB 板（C3、C6）使用 `partitions-4mb.csv`，8 MB 板（C5、S3）使用 `partitions-8mb.csv`，由 `sdkconfig.defaults.<芯片>` 自动选择。

## 5. 运行

每次测一块板，结果以板名记入 `results/comparison/`：

```sh
python tools/run_flash.py --board C3 --note "USB powered, desk"
python tools/run_flash.py --board C5 --port /dev/cu.usbmodem1201
```

脚本通过串口把 Wi-Fi 信息存进板子并读取 IP 地址，之后全部通过网络完成：清空文件区、安装资源包、OTA 升级、读回校验。每块板约 1–3 分钟，耗时主要取决于 Wi-Fi 速度和装入的数据量。用同一个板名再测一次会替换该板的旧结果。

| 参数 | 作用 | 默认值 |
| --- | --- | --- |
| `--board 名称` | 该板在对比中的名称 | 必填 |
| `--port` | 串口 | 唯一的 `/dev/cu.usbmodem*` |
| `--note` | 该板测试条件的文字记录 | 空 |
| `--out` | 对比目录 | `results/comparison` |
| `--replot` | 只用已保存的数据重新生成表格和图 | — |

测试结束后，浏览器打开 `http://<板子 IP>/` 可以看到仪表盘：Flash 分区图、已装资源包，以及用板上中文字体显示的示例文字。

## 6. 结果文件

| 文件 | 内容 |
| --- | --- |
| `summary.md` | 每块板的固件大小、文件区容量与剩余、各资源包是否装下、OTA 结果与读回校验 |
| `flash-map.svg` | 所有板的 Flash 分布并排对比 |
| `<名称>.json` | 分区表、安装前后与升级后的板卡信息、每个资源包的结果、测试条件 |

## 7. 网页接口

| 接口 | 作用 |
| --- | --- |
| `GET /api/info` | 芯片、固件版本、当前运行分区、分区表、文件区容量与剩余、IP |
| `POST /api/format` | 清空文件区 |
| `PUT` / `GET` / `DELETE /api/files/<路径>` | 写入、读取、删除文件；空间不足时写入返回 507 |
| `POST /api/ota` | 上传固件到空闲 OTA 分区并重启 |
| `GET /<路径>` | 从文件区 `www/` 提供网页文件，存在 `.gz` 版本时直接以 gzip 发送 |

串口命令 `info` 输出同样的板卡信息，`wifi <名称> [密码]` 保存 Wi-Fi 并连接。

## 8. 结论口径

- 本实验比较的是**保留双分区 OTA 后的文件空间**：同样的固件和升级能力下，还能装多少网页、字体、图标等本地资源。
- XIAO ESP32-S3 同样有 8 MB Flash，结果与 C5 同属一组；对比重点是 4 MB 的 C3、C6。
- 更大的 Flash 带来存储空间，不代表 CPU 或网页运行速度更快。
