# 03: 240 MHz CPU — Fixed-Point Fractal Rendering Benchmark

Four XIAO boards (C3, C5, C6, S3) render the exact same four 320×240 fractal scenes on-chip using pure integer fixed-point math. Every chip produces pixel-identical images; only the execution time differs.

Full write-up: **[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)** · Pre-recorded results: [`results/summary.md`](results/summary.md)

## 1. Experiment Design

| Item | Configuration |
| --- | --- |
| Scenes | Four 320×240 fractals (`mandelbrot`, `seahorse-valley`, `elephant-valley`, `julia`), max 255 iterations/pixel, **26,230,366** total iterations |
| Arithmetic | Pure 32-bit `Q7.24` integer fixed-point math — bit-exact across architectures with zero hardware FPU bias |
| Scheduling Modes | `seq`: single FreeRTOS task renders all four scenes sequentially. `par`: four concurrent tasks render one scene each (spread across both cores on the dual-core S3) |
| Clean Timing | Timed on-board via `esp_timer` with Wi-Fi off, task watchdog disabled, and zero serial output during timing |
| Clocks & Flags | Identical C source, ESP-IDF v6.1.1, `-O2` optimization; default max clocks: 160 MHz on C3/C6, 240 MHz on C5/S3 |
| Runs & Checksum | 5 runs per mode (reporting median and min–max); working buffer is a single 320-byte stack row so PSRAM size has zero effect |

## 2. Requirements

| Item | Requirement |
| --- | --- |
| Hardware | XIAO ESP32-C3, XIAO ESP32-C5, XIAO ESP32-C6, XIAO ESP32-S3, USB-C data cable |
| Firmware SDK | ESP-IDF **v6.1.1** |
| Host | Python + `pyserial` bundled in the ESP-IDF Python environment |

## 3. Build & Flash

```sh
get_idf
idf.py --version            # Expect ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  B="$HOME/.cache/a1-cpu/$t"
  idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" build
done

B="$HOME/.cache/a1-cpu/esp32c5"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

## 4. Run

```sh
python tools/run_cpu.py --board C3 --out results --note "USB powered, desk"
python tools/run_cpu.py --board S3 --port /dev/cu.usbmodem2101 --out results
```

| Flag | Description | Default |
| --- | --- | --- |
| `--board NAME` | Board label in the comparison table | Required |
| `--port` | Serial port | Auto-detect single `/dev/cu.usbmodem*` |
| `--runs` | Repetitions per scheduling mode | `5` |
| `--note` | Test condition note | Empty |
| `--out` | Output directory | `results/comparison` |
| `--replot` | Regenerate summary table and SVG plot from saved data only | — |

## 5. Output Files

Archived in [`results/`](results/):

| File | Description |
| --- | --- |
| `summary.md` | Median and min–max render times for `seq` & `par`, speedup vs. C3, and pixel CRC verification |
| `times.svg` | Bar chart comparing `seq` and `par` render times across all four boards |
| `<NAME>.png` | 2×2 composite PNG of the four fractals rendered by that board |
| `<NAME>.json` | Chip metadata, per-run and per-scene microsecond timings, CRC32 hashes, and iteration counts |

## 6. Interactive Serial Commands

| Command | Description |
| --- | --- |
| `info` | Print chip model, core count, CPU clock, and scene dimensions |
| `bench mode=seq runs=5` | Run timed benchmark (`mode=par` runs four concurrent tasks) |
| `dump <0-3>` | Render one scene untimed and output pixels framed by `IMG_BEGIN` / `R <base64>` / `IMG_END` |
