# 04: Native CAN FD — Classic CAN vs. CAN FD Physical Bus Throughput

A single XIAO ESP32-C5 uses both of its built-in TWAI CAN FD controllers—each wired to a physical CAN transceiver—to form a real two-node differential twisted-pair bus. Node A transmits a 64 KiB pseudo-random data block to Node B across Classic CAN and CAN FD modes, timing end-to-end completion and verifying every received byte by CRC32.

*(C3, C6, and S3 integrate Classic-CAN-only controllers without CAN FD support.)*

Full write-up: **[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)** · Pre-recorded results: [`results/summary.md`](results/summary.md)

## 1. Experiment Design

| Item | Configuration |
| --- | --- |
| Payload Block | 64 KiB (`65,536 bytes`) deterministic pseudo-random block |
| Three Bus Modes | `classic`: 8 B/frame Classic CAN. `fd_nobrs`: 64 B/frame CAN FD at arbitration rate. `fd`: 64 B/frame CAN FD with Bit-Rate Switch (BRS) in the data phase |
| Bit Rates | 500 kbps arbitration; 2 Mbps (default) and 5 Mbps FD data phase |
| Frame Header | 29-bit extended ID encoding packet sequence number so 100% of the frame payload carries data |
| Timing & Audit | Timed on-chip via `esp_timer` from first transmit to final receive; verifies missing/duplicate/out-of-order frames, full block CRC32, and hardware `tec`/`rec` error counters |

Splitting `fd_nobrs` and `fd` isolates how much speedup comes from 8× larger frames alone versus switching the data phase to 2 Mbps or 5 Mbps.

## 2. Requirements

| Item | Requirement |
| --- | --- |
| Board | 1× XIAO ESP32-C5 + USB-C data cable |
| Transceivers | 2× Adafruit CAN Pal (`#5708`, NXP `TJA1051T/3`) |
| Wiring | Jumper wires + twisted pair (`CANH` / `CANL`, < 1 m), both 120 Ω termination switches **ON** |
| Firmware SDK | ESP-IDF **v6.1.1** |

## 3. Wiring

Unplug USB before wiring:

| CAN Pal A (TX Node) | XIAO ESP32-C5 | CAN Pal B (RX Node) | XIAO ESP32-C5 |
| --- | --- | --- | --- |
| `Vcc` | `3V3` | `Vcc` | `3V3` |
| `GND` | `GND` | `GND` | `GND` |
| `TX` | `D4` (`GPIO23`) | `TX` | `D8` (`GPIO8`) |
| `RX` | `D5` (`GPIO24`) | `RX` | `D9` (`GPIO9`) |
| `SLNT` | Floating | `SLNT` | Floating |

Connect `CANH` to `CANH` and `CANL` to `CANL` between the two transceivers using a twisted pair, and switch **Termination ON** (120 Ω) at both ends.

## 4. Build & Flash

```sh
get_idf
idf.py --version            # Expect ESP-IDF v6.1.1

B="$HOME/.cache/a1-canfd/esp32c5"
idf.py -B "$B/build" -DIDF_TARGET=esp32c5 -DSDKCONFIG="$B/sdkconfig" build
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

## 5. Run

```sh
python tools/run_canfd.py --board C5 --out results --note "two CAN Pal, short twisted pair"
python tools/run_canfd.py --board C5-5M --data 5000000 --out results   # 5 Mbps data phase
```

| Flag | Description | Default |
| --- | --- | --- |
| `--board NAME` | Label in the comparison table | Required |
| `--port` | Serial port | Auto-detect single `/dev/cu.usbmodem*` |
| `--bytes` | Block size (multiple of 64, up to 131072) | `65536` |
| `--arb` | Arbitration bit rate (bps) | `500000` |
| `--data` | CAN FD data-phase bit rate (bps) | `2000000` |
| `--runs` | Repetitions per mode | `3` |
| `--note` | Test condition note | Empty |
| `--out` | Output directory | `results/comparison` |
| `--replot` | Regenerate summary table and SVG plot from saved data only | — |

## 6. Output Files & Serial Commands

Archived in [`results/`](results/): `summary.md`, `times.svg`, and `<NAME>.json`.

Interactive serial commands: `info` (controller capabilities & pins), `probe` (single-node bus wiring sanity check — `err_flags == 16` confirms clean ACK-only error with valid transceiver wiring), and `run mode=fd bytes=65536 arb=500000 data=2000000`.
