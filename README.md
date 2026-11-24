# XIAO ESP32-C3 / C5 / C6 / S3 Real-World Benchmark Suite

Companion firmware, host automation scripts, and measured benchmark data for the Hackster.io comparison article:

**[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)**

Each subdirectory is a standalone ESP-IDF project. All four Seeed Studio XIAO boards (`XIAO ESP32-C3`, `XIAO ESP32-C5`, `XIAO ESP32-C6`, and `XIAO ESP32-S3`) run the exact same C application source on each real-world task so measured differences reflect actual silicon and board capabilities rather than code differences.

| Test | Directory | Real-World Task | Measured Results |
| --- | --- | --- | --- |
| 1. 8 MB PSRAM | [01-psram-recorder](01-psram-recorder/README.md) | 60-second 2 kHz ring-buffer fault recorder | [Summary & Plots](01-psram-recorder/results/summary.md) |
| 2. 8 MB Flash + Dual OTA | [02-flash-ota](02-flash-ota/README.md) | Web UI & CJK font capacity alongside Dual-OTA slots | [Summary & Plots](02-flash-ota/results/summary.md) |
| 3. 240 MHz CPU | [03-cpu-render](03-cpu-render/README.md) | Fixed-point fractal rendering (single-task vs. 4-task) | [Summary & Plots](03-cpu-render/results/summary.md) |
| 4. Native CAN FD | [04-canfd](04-canfd/README.md) | Classic CAN vs. CAN FD 64 KiB physical bus transfer | [Summary & Plots](04-canfd/results/summary.md) |
| 5. Wi-Fi 6 Power (iTWT) | [05-wifi6-itwt](05-wifi6-itwt/README.md) | Connected current & latency across 2.4 GHz / 5 GHz & TWT | [Summary & Plots](05-wifi6-itwt/results/summary.md) |

## General Conventions

- **Toolchain:** Built with **ESP-IDF v6.1.1** across all four targets (`esp32c3`, `esp32c5`, `esp32c6`, `esp32s3`).
- **Wi-Fi Credentials:** Networked benchmarks read `WIFI_SSID`, `WIFI_PASSWORD`, and `WIFI_SSID_5G` from `.env` in this root directory (copy `.env.example` to `.env`).
- **Out-of-Tree Builds:** Build each chip in its own ASCII path via `idf.py -B "$HOME/.cache/<project>/<target>/build" -DIDF_TARGET=<target> -DSDKCONFIG=...` so all four target builds can coexist cleanly.
- **Board-Level Measurements:** All measurements are taken on retail Seeed Studio XIAO boards and include board-level LDO, antenna, and PCB characteristics.
- **Recorded Data & Figures:** Pre-recorded raw logs, JSON summaries, SVGs, and conclusion graphics are archived under each project's `results/` directory.

## Articles

- **Hackster.io (Published):** [Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)
- **Local Manuscripts:** [English Markdown](XIAO-ESP32-C5-In-Depth-Comparison.md) · [Chinese Markdown](XIAO-ESP32-C5-深度对比评测.md)
