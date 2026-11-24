# 01: 8 MB PSRAM — 60-Second Ring-Buffer Fault Recorder

Four XIAO boards (C3, C5, C6, S3) run identical firmware to record the exact same deterministic simulated 3-axis vibration stream in real time, with a sharp fault spike injected at the 10-second mark. After 60 seconds of continuous recording, the host computer dumps whatever history survived in each board's ring buffer to check whether the 10-second fault is still intact.

Full write-up: **[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)** · Pre-recorded results: [`results/summary.md`](results/summary.md)

## 1. Experiment Design

| Item | Configuration |
| --- | --- |
| Stream | 2,000 samples/s, three little-endian `int16` axes (6 bytes/sample): X = 50 Hz, Y = 120 Hz, Z = 1 g + deterministic pseudo-random noise |
| Fault Spike | 10 ms high-amplitude spike starting at exactly `10.000 s` |
| Full History | `60 s × 2,000 samples/s × 6 B = 720,000 bytes` |
| Buffer Policy | Circular ring buffer requesting the full 60 s; falls back to the largest available block and wraps around over the oldest samples when full |
| Memory Selection | Uses external PSRAM when available (`C5`, `S3`); falls back to internal SRAM on `C3` and `C6` while reserving 32 KiB for the system |
| Dump & Verify | Host receives only progress ticks during recording (zero host buffering); full dump after 60 s is verified by `CRC32` |

## 2. Requirements

| Item | Requirement |
| --- | --- |
| Hardware | XIAO ESP32-C3, XIAO ESP32-C5, XIAO ESP32-C6, XIAO ESP32-S3 (8 MB PSRAM variant such as Sense or Plus), USB-C data cable |
| Firmware SDK | ESP-IDF **v6.1.1** (ESP32-C5 v1.0 silicon requires v5.5.2+) |
| Host | Python + `pyserial` bundled in the ESP-IDF Python environment |

## 3. Build & Flash

Keep separate ASCII build directories per target so all four builds can coexist:

```sh
get_idf
idf.py --version            # Expect ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  B="$HOME/.cache/a1-psram/$t"
  idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" build
done
```

Find the serial port (`ls /dev/cu.usbmodem*`) and flash the corresponding target (**replaces existing firmware on the board**):

```sh
B="$HOME/.cache/a1-psram/esp32c5"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

Target-specific flash size, CPU clock, and PSRAM settings live in `sdkconfig.defaults.<target>`: 4 MB Flash / 160 MHz for C3 & C6; 8 MB Flash / 240 MHz with PSRAM enabled for C5 & S3 (Octal mode on S3).

## 4. Run

Each run records one board and updates the comparison table and waveform plot (`--out results` updates the archived folder directly; default `--out` is `results/comparison`):

```sh
python tools/run_psram.py --board C3 --out results --note "USB powered, desk"
python tools/run_psram.py --board C5 --port /dev/cu.usbmodem1201 --out results --note "USB powered, desk"
python tools/run_psram.py --board C5-internal --port /dev/cu.usbmodem1201 --out results --mem internal
```

| Flag | Description | Default |
| --- | --- | --- |
| `--board NAME` | Board label in the comparison table (e.g., `C3`, `C5`, `C5-internal`) | Required |
| `--port` | Serial port | Auto-detect single `/dev/cu.usbmodem*` |
| `--mem auto\|internal` | `internal` forces PSRAM boards to use internal SRAM only as a control run | `auto` |
| `--seconds` | Recording duration in seconds (keep identical across boards) | `60` |
| `--note` | Test condition note | Empty |
| `--out` | Output directory | `results/comparison` |
| `--replot` | Regenerate summary table and SVG plot from saved data only | — |

## 5. Output Files

Archived in [`results/`](results/):

| File | Description |
| --- | --- |
| `summary.md` | Buffer size, retained duration/window, whether the 10 s fault survived, and dump time |
| `waveforms.svg` | Side-by-side X-axis 0–60 s history across all boards (overwritten segments shaded gray) |
| `<NAME>.bin` | Raw dumped samples (little-endian `int16` X/Y/Z in chronological order) |
| `<NAME>.json` | Chip info, heap usage, board stats, `note`, and `recorded_at` timestamp |

## 6. Interactive Serial Commands

| Command | Description |
| --- | --- |
| `info` | Print chip model, revision, CPU clock, and free PSRAM / internal SRAM |
| `record seconds=60 mem=auto` | Run timed capture (`mem=internal` restricts to internal SRAM) |
| `dump` | Stream retained samples framed by `DUMP_BEGIN` / `D <base64>` / `DUMP_END` with CRC32 |
