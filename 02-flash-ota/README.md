# 02: 8 MB Flash + Dual OTA — Usable Storage After Reserving Update Slots

Four XIAO boards (C3, C5, C6, S3) run identical web-dashboard firmware under the same partitioning rule: two 1.5 MB OTA app slots (`ota_0` and `ota_1`), with the remaining flash formatted as a FAT `storage` partition. The host installs real open-source frontend asset packs in fixed order to see what fits, pushes a V2 firmware update over HTTP OTA, and then reads back every stored file to verify its SHA-256 hash.

Full write-up: **[Is the $6.90 XIAO ESP32-C5 Worth It Over C3, C6, and S3?](https://www.hackster.io/limengdu0117/is-the-6-90-xiao-esp32-c5-worth-it-over-c3-c6-and-s3-3cb24e)** · Pre-recorded results: [`results/summary.md`](results/summary.md)

## 1. Experiment Design

| Item | Configuration |
| --- | --- |
| Firmware | Wi-Fi HTTP file server + `/api/ota` endpoint; V1 and V2 are compiled from the exact same source with different version strings |
| Partitions | `ota_0` & `ota_1` = 1.5 MB each; remaining flash = FAT `storage` (~0.9 MB raw on 4 MB boards, ~4.9 MB raw on 8 MB boards) |
| Asset Packs | Uploaded in fixed order (table below); text assets stored gzip-compressed, font files stored as-is |
| Full-Disk Rollback | When a write returns HTTP `507 Insufficient Storage`, the partial file is unlinked and the current pack is rolled back cleanly |
| OTA Update | Host `POST`s V2 binary to `/api/ota`; board writes the inactive slot and reboots into V2 |
| Verification | Every stored file is read back over HTTP and checked against its SHA-256 hash, plus `GET /` HTTP 200 check |

| Order | Pack | Contents | Stored Size |
| --- | --- | --- | ---: |
| 1 | `dashboard` | Dashboard UI + Chart.js 4.4.1 | ~71 KiB |
| 2 | `icons` | Font Awesome Free 6.5.1 solid & regular icons | ~202 KiB |
| 3 | `latin-font` | Inter UI font (regular + bold) | ~46 KiB |
| 4 | `cjk-regular` | Noto Sans SC Chinese font (regular) | ~3.4 MiB |
| 5 | `cjk-bold` | Noto Sans SC Chinese font (bold) | ~3.5 MiB |

All third-party web assets are fetched from pinned npm releases and verified by SHA-256 checksums, with license files stored alongside them under `www/licenses/`.

## 2. Requirements

| Item | Requirement |
| --- | --- |
| Hardware | XIAO ESP32-C3, XIAO ESP32-C5, XIAO ESP32-C6, XIAO ESP32-S3, USB-C data cable |
| Network | 2.4 GHz Wi-Fi on the same LAN as the host computer (C5 also supports 5 GHz) |
| Firmware SDK | ESP-IDF **v6.1.1** |
| Credentials | Set `WIFI_SSID` and `WIFI_PASSWORD` in `../.env` (see `../.env.example`) |

## 3. Fetch Frontend Assets

```sh
python3 tools/fetch_assets.py
```

Assets are cached under `assets/cache/` (gitignored) and skipped on subsequent runs once verified.

## 4. Build & Flash

Build both `1.0.0` (flashed via USB) and `2.0.0` (pushed over HTTP OTA) per target:

```sh
get_idf
idf.py --version            # Expect ESP-IDF v6.1.1

for t in esp32c3 esp32c5 esp32c6 esp32s3; do
  for v in 1.0.0 2.0.0; do
    B="$HOME/.cache/a1-flash/$t/v$v"
    idf.py -B "$B/build" -DIDF_TARGET=$t -DSDKCONFIG="$B/sdkconfig" -DPROJECT_VER=$v build
  done
done
```

Flash V1 over USB (**replaces existing firmware on the board**):

```sh
B="$HOME/.cache/a1-flash/esp32c5/v1.0.0"
idf.py -B "$B/build" -DSDKCONFIG="$B/sdkconfig" -p /dev/cu.usbmodemXXXX flash
```

`sdkconfig.defaults.<target>` automatically selects `partitions-4mb.csv` for C3/C6 and `partitions-8mb.csv` for C5/S3.

## 5. Run

```sh
python tools/run_flash.py --board C3 --out results --note "USB powered, desk"
python tools/run_flash.py --board C5 --port /dev/cu.usbmodem1201 --out results
```

The script provisions Wi-Fi over serial to obtain the board's IP address, then runs everything over HTTP: format `storage`, upload asset packs, trigger V1→V2 OTA, and verify readback hashes. Opening `http://<board-ip>/` afterward shows the live local dashboard.

| Flag | Description | Default |
| --- | --- | --- |
| `--board NAME` | Board label in the comparison table | Required |
| `--port` | Serial port | Auto-detect single `/dev/cu.usbmodem*` |
| `--note` | Test condition note | Empty |
| `--out` | Output directory | `results/comparison` |
| `--replot` | Regenerate summary table and SVG plot from saved data only | — |

## 6. Output Files

Archived in [`results/`](results/):

| File | Description |
| --- | --- |
| `summary.md` | App binary size, formatted storage total/free, fitted packs, OTA timing, and readback verification |
| `flash-map.svg` | Side-by-side flash layout and file capacity map across all four boards |
| `<NAME>.json` | Partition map, pre/post-OTA board metadata, per-pack upload stats, and test notes |

## 7. HTTP Endpoints

| Endpoint | Description |
| --- | --- |
| `GET /api/info` | Chip, firmware version, active OTA partition, partition map, storage total/free, IP |
| `POST /api/format` | Format/clear the FAT file area |
| `PUT` / `GET` / `DELETE /api/files/<path>` | Upload, read, or delete files (returns `507` when out of space) |
| `POST /api/ota` | Stream new firmware binary to inactive OTA slot and reboot |
| `GET /<path>` | Serve static assets from `www/` (automatically serves `.gz` when present) |
