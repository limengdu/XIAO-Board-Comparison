# 05: Dual-Band Wi-Fi 6 Power — DTIM Power Save vs. Target Wake Time (iTWT)

Each XIAO board stays associated to a live Wi-Fi router performing the exact same connected workload while switching power-save modes. A Nordic Power Profiler Kit II (PPK2) powers the backside battery pads with USB completely unplugged to log true connected current alongside random ping latency.

- **C5 & C6 (Wi-Fi 6):** Tested across DTIM power save and negotiated iTWT schedules; **C5** is tested on both **2.4 GHz** and **5 GHz**.
- **C3 & S3 (Wi-Fi 4):** Tested in 2.4 GHz DTIM power save as baselines.

Full write-up: **[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)** · Pre-recorded results: [`results/summary.md`](results/summary.md)

## 1. Experiment Design

| Item | Configuration |
| --- | --- |
| Live Workload | Stay associated; answer random host `PING` probes every 3–7 s with `PONG`; send a 64-byte UDP telemetry packet every 10 s |
| `ps` | Standard 802.11 modem sleep (waking on router DTIM beacons) |
| `twt<ms>` | Individual Wi-Fi 6 TWT agreement waking every `<ms>` ms with ESP-IDF's default ~65.3 ms wake window |
| `twt<ms>w<n>` | Same TWT interval, narrowing each wake window to `<n>` ms (`1–65`, e.g., `w8` ≈ 8.2 ms) |
| Power Management | Automatic Light Sleep and Dynamic Frequency Scaling (DFS) enabled across all phases |
| Measurement | PPK2 source mode at `3.7 V` on `BAT+`/`BAT-` (`4.2 V` on S3 for its ~730 mA Wi-Fi startup peak), USB unplugged, `100 kS/s` sampling |
| Trimming & Metrics | Mean current trimmed (`+1.0 s` after `BEGIN`, `-0.5 s` before `END`); 3 s mid-phase trace + light-sleep floor + Ping median/P90/loss |

Default 60-second phases per target:

| Board | Default Test Plan |
| --- | --- |
| C5 | `2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8,5g/ps,5g/twt500,5g/twt2000,5g/twt2000w8` |
| C6 | `2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8` |
| C3, S3 | `2g/ps` |

## 2. Requirements & Wiring

| Item | Requirement |
| --- | --- |
| Hardware | XIAO board under test, Nordic PPK2, two thin leads soldered to backside `BAT+` / `BAT-` pads |
| Router | Wi-Fi 6 (802.11ax) router with TWT enabled and separate 2.4 GHz / 5 GHz SSIDs (`CONFIG_ESP_WIFI_PASSIVE_HIDDEN_AP_SUPPORT` is enabled for hidden DFS channels) |
| Credentials | Set `WIFI_SSID`, `WIFI_SSID_5G`, and `WIFI_PASSWORD` in `../.env` (see `../.env.example`) |
| Software | ESP-IDF **v6.1.1** + local `.venv` containing `ppk2-api` and `pyserial` |

Wire PPK2 `VOUT` → XIAO `BAT+` and PPK2 `GND` → XIAO `BAT-`. During measurement XIAO's USB cable is unplugged so only PPK2 powers the board.

## 3. Build & Flash

```sh
get_idf
idf.py --version            # Expect ESP-IDF v6.1.1

T=esp32c5                   # esp32c3 / esp32c5 / esp32c6 / esp32s3
B="$HOME/.cache/a1-wifi6/$T"
idf.py -B "$B/build" -DIDF_TARGET=$T -DSDKCONFIG="$B/sdkconfig" build
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

## 4. Run

Verify network connectivity over USB first without PPK2 (`--dry-run`):

```sh
.venv/bin/python tools/run_power.py --board C5 --dry-run --secs 20
```

Full unplugged PPK2 measurement:

```sh
.venv/bin/python tools/run_power.py --board C5 --out results --note "ASUS Wi-Fi 6 router, 1 m"
```

1. While USB is connected, the script writes Wi-Fi credentials, host IP, and the phase schedule to NVS and sets `autorun=1`.
2. Unplug XIAO's USB cable when prompted; once `/dev/cu.usbmodem*` disappears, PPK2 enables battery-pad power and begins 100 kS/s capture.
3. UDP `BEGIN`/`END` markers align each phase automatically; once all phases complete, the board enters deep sleep and results are written out.

| Flag | Description | Default |
| --- | --- | --- |
| `--board NAME` | Board label in the comparison table | Required |
| `--port` | Board serial port | Auto-detect single `/dev/cu.usbmodem*` |
| `--ppk-port` | PPK2 serial port | Auto-detect |
| `--host` | Host LAN IPv4 address | Primary route interface IP |
| `--plan` | Comma-separated phase list | Target default (Section 1) |
| `--secs` | Duration per phase (`10–600` s) | `60` |
| `--mv` | PPK2 supply voltage in mV (`4200` for S3) | `3700` |
| `--dry-run` | Test phases over USB without PPK2 | Off |
| `--out` | Output directory | `results/comparison` |
| `--replot` | Regenerate report and SVG plots from saved data only | Off |

## 5. Output Files

Archived in [`results/`](results/): [`summary.md`](results/summary.md) (mean current, savings vs. DTIM power save, light-sleep floor, Ping RTT median/P90, packet loss), `current.svg` (mean current bar chart), and `traces.svg` (3-second current waveforms).
