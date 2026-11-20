#!/usr/bin/env python3
"""Run the Flash + OTA experiment on one XIAO board and add it to the comparison.

The board must run the V1 firmware of this project. The script:
  1. joins Wi-Fi (credentials from the A1 .env file) and reads the flash layout,
  2. erases the file area and installs the resource packs from assets/manifest.json in order;
     a pack that does not fit is removed again and the remaining packs are skipped,
  3. sends the V2 firmware over HTTP OTA and waits for the board to boot it,
  4. reads every stored file back and checks its SHA-256, and requests the dashboard page.

Results go to results/comparison/<label>.json; summary.md and flash-map.svg are rebuilt for every
board recorded so far. Run tools/fetch_assets.py once first. Run inside an ESP-IDF shell (get_idf),
which already provides pyserial.

Examples:
    python tools/run_flash.py --board C3
    python tools/run_flash.py --board C5 --port /dev/cu.usbmodem1201
    python tools/run_flash.py --replot
"""

import argparse
import glob
import gzip
import hashlib
import http.client
import json
import sys
import time
import urllib.parse
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RESULTS_DIR = ROOT / "results"
DEFAULT_OUT = RESULTS_DIR / "comparison"
RESOLVED = ROOT / "assets" / "cache" / "resolved.json"
ENV_FILE = ROOT.parent / ".env"
V2_BIN = Path.home() / ".cache" / "a1-flash" / "{target}" / "v2.0.0" / "build" / "flash_ota_bench.bin"


class Console:
    """Line-oriented access to the firmware REPL without resetting the board."""

    def __init__(self, port: str):
        import serial

        self.ser = serial.Serial()
        self.ser.port = port
        self.ser.baudrate = 115200
        self.ser.timeout = 0.1
        self.ser.dtr = False
        self.ser.rts = False
        self.ser.open()
        self.pending = b""

    def close(self):
        self.ser.close()

    def command(self, line: str, tag: str, timeout_s: float):
        """Send one command and return the JSON object of the first "<tag> {...}" line."""
        self.ser.reset_input_buffer()
        self.pending = b""
        self.ser.write((line + "\r\n").encode())
        marker = tag + " {"
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            self.pending += self.ser.read(4096)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                idx = text.find(marker)
                if idx >= 0:
                    return json.loads(text[idx + len(tag) + 1:])
        raise TimeoutError(f"no {tag} reply within {timeout_s:.0f}s for: {line.split()[0]}")


class Board:
    """HTTP client for the board with one keep-alive connection."""

    def __init__(self, host: str):
        self.host = host
        self.conn = None

    def request(self, method: str, path: str, body: bytes = None, timeout: float = 60):
        for attempt in range(2):
            try:
                if self.conn is None:
                    self.conn = http.client.HTTPConnection(self.host, 80, timeout=timeout)
                self.conn.request(method, path, body=body)
                resp = self.conn.getresponse()
                return resp.status, resp.read()
            except (OSError, http.client.HTTPException):
                if self.conn:
                    self.conn.close()
                self.conn = None
                if attempt:
                    raise

    def json(self, method: str, path: str, body: bytes = None, timeout: float = 60):
        status, data = self.request(method, path, body, timeout)
        return status, json.loads(data) if data else {}

    def info(self):
        status, info = self.json("GET", "/api/info", timeout=10)
        if status != 200:
            raise RuntimeError(f"/api/info returned {status}")
        return info


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"expected exactly one board on /dev/cu.usbmodem*, found {ports or 'none'}; pass --port")
    return ports[0]


def load_env():
    if not ENV_FILE.exists():
        sys.exit(f"missing {ENV_FILE}; copy .env.example and fill in WIFI_SSID and WIFI_PASSWORD")
    env = {}
    for line in ENV_FILE.read_text().splitlines():
        key, sep, value = line.partition("=")
        if sep and not key.strip().startswith("#"):
            env[key.strip()] = value.strip().strip('"').strip("'")
    if not env.get("WIFI_SSID"):
        sys.exit(f"WIFI_SSID is empty in {ENV_FILE}")
    return env["WIFI_SSID"], env.get("WIFI_PASSWORD", "")


def quote_arg(value: str) -> str:
    return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'


def join_wifi(con: Console, info, timeout_s=40):
    """Return board info once it has an IP address, sending credentials when needed."""
    if info.get("ip"):
        return info
    ssid, password = load_env()
    res = con.command(f"wifi {quote_arg(ssid)} {quote_arg(password)}", "RESULT", 10)
    if not res.get("ok"):
        sys.exit(f"wifi command failed: {res}")
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        time.sleep(2)
        info = con.command("info", "INFO", 5)
        if info.get("ip"):
            return info
    sys.exit(f"board did not join Wi-Fi '{ssid}' within {timeout_s}s; check that the network is on a band the chip supports")


def pack_payload(pack):
    """[(dest, bytes)] for one pack, gzip-compressed where the manifest asks for it."""
    out = []
    for f in pack["files"]:
        data = Path(f["src"]).read_bytes()
        if f["gzip"]:
            out.append((f["dest"] + ".gz", gzip.compress(data, compresslevel=9, mtime=0)))
        else:
            out.append((f["dest"], data))
    return out


def file_url(dest: str) -> str:
    return "/api/files/" + urllib.parse.quote(dest)


def install_packs(board: Board, packs):
    """Upload packs in order; returns (pack results, {dest: sha256} of everything stored)."""
    stored, results, full = {}, [], False
    for pack in packs:
        payload = pack_payload(pack)
        entry = {"id": pack["id"], "title": pack["title"], "files": len(payload),
                 "raw_bytes": sum(Path(f["src"]).stat().st_size for f in pack["files"]),
                 "stored_bytes": sum(len(d) for _, d in payload)}
        if full:
            entry["status"] = "skipped"
            results.append(entry)
            continue
        done, t0 = [], time.monotonic()
        for dest, data in payload:
            status, _ = board.request("PUT", file_url(dest), data, timeout=120)
            if status == 507:
                full = True
                break
            if status != 201:
                raise RuntimeError(f"upload {dest} returned {status}")
            done.append((dest, data))
        if full:
            for dest, _ in done:
                board.request("DELETE", file_url(dest))
            entry["status"] = "does_not_fit"
            entry["files_before_full"] = len(done)
        else:
            entry["status"] = "installed"
            stored.update({dest: hashlib.sha256(data).hexdigest() for dest, data in done})
        entry["seconds"] = round(time.monotonic() - t0, 1)
        entry["fs_free_after"] = board.info()["fs_free"]
        print(f"  {pack['id']:<12} {entry['status']:<13} {entry['stored_bytes'] / 1024:7.0f} KiB  "
              f"free after {entry['fs_free_after'] / 1024:7.0f} KiB")
        results.append(entry)
    return results, stored


def run_ota(board: Board, before, bin_path: Path, timeout_s=90):
    image = bin_path.read_bytes()
    t0 = time.monotonic()
    status, res = board.json("POST", "/api/ota", image, timeout=120)
    if status != 200 or not res.get("ok"):
        raise RuntimeError(f"OTA upload failed: {status} {res}")
    board.conn.close()
    board.conn = None
    deadline = time.monotonic() + timeout_s
    time.sleep(3)
    while time.monotonic() < deadline:
        try:
            after = board.info()
            if after["version"] != before["version"]:
                return {"image_bytes": len(image), "seconds": round(time.monotonic() - t0, 1),
                        "from": f"{before['version']} on {before['running']}",
                        "to": f"{after['version']} on {after['running']}", "ok": True}, after
        except (OSError, http.client.HTTPException, RuntimeError, ValueError):
            pass
        time.sleep(2)
    raise RuntimeError("board did not come back with the new firmware")


def verify(board: Board, stored):
    bad = []
    for dest, digest in stored.items():
        status, data = board.request("GET", file_url(dest))
        if status != 200 or hashlib.sha256(data).hexdigest() != digest:
            bad.append(dest)
    page_status, _ = board.request("GET", "/")
    return {"files_checked": len(stored), "files_bad": bad, "dashboard_status": page_status,
            "ok": not bad and page_status == 200}


def run_board(label, port, note, out_dir: Path):
    if not RESOLVED.exists():
        sys.exit("run tools/fetch_assets.py first")
    packs = json.loads(RESOLVED.read_text())["packs"]
    con = Console(port)
    try:
        info = join_wifi(con, con.command("info", "INFO", 5))
    finally:
        con.close()
    bin_path = Path(str(V2_BIN).format(target=info["target"]))
    if not bin_path.exists():
        sys.exit(f"missing V2 image {bin_path}")
    print(f"[{label}] {info['target']} {info['flash_bytes'] // 1048576} MB flash, firmware {info['version']} "
          f"on {info['running']}, {info['ip']} ({info['rssi']} dBm)")

    board = Board(info["ip"])
    status, before = board.json("POST", "/api/format", timeout=120)
    if status != 200:
        sys.exit(f"format failed: {status} {before}")
    print(f"[{label}] file area {before['fs_total'] / 1024:.0f} KiB after format")
    packs_res, stored = install_packs(board, packs)
    installed = board.info()
    print(f"[{label}] OTA {before['version']} -> V2 ({bin_path.stat().st_size / 1024:.0f} KiB)")
    ota, after = run_ota(board, installed, bin_path)
    print(f"[{label}] now {ota['to']} after {ota['seconds']} s")
    check = verify(board, stored)
    print(f"[{label}] read back {check['files_checked']} files, bad {len(check['files_bad'])}, "
          f"dashboard HTTP {check['dashboard_status']}")

    meta = {"label": label, "port": port, "note": note, "recorded_at": datetime.now().isoformat(timespec="seconds"),
            "before": before, "installed": installed, "after": after, "packs": packs_res, "ota": ota,
            "verify": check}
    (out_dir / f"{label}.json").write_text(json.dumps(meta, indent=2))


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def regions(run):
    """Flash regions of one board as [(name, bytes, color)] in address order."""
    info = run["installed"]
    parts = {p["label"]: p for p in info["partitions"]}
    storage = parts["storage"]
    running = run["before"]["running"]
    idle = "ota_1" if running == "ota_0" else "ota_0"
    system = parts["ota_0"]["offset"]
    used = info["fs_total"] - info["fs_free"]
    return [
        ("Bootloader, table, NVS", system, "#98a2b3"),
        ("Running firmware", info["app_bytes"], "#1f6fb2"),
        ("Firmware slot headroom", parts[running]["size"] - info["app_bytes"], "#b2ddff"),
        ("Update slot", parts[idle]["size"], "#84caff"),
        ("Files stored", used, "#f79009"),
        ("Files free", info["fs_free"], "#fedf89"),
        ("FAT overhead", storage["size"] - info["fs_total"], "#d0d5dd"),
        ("Unpartitioned", info["flash_bytes"] - storage["offset"] - storage["size"], "#ffffff"),
    ]


def draw_svg(runs, path: Path):
    width, left, right, row_h, top = 1100, 150, 30, 70, 60
    plot_w = width - left - right
    max_flash = max(r["installed"]["flash_bytes"] for r in runs)
    legend = regions(runs[0])[:-1]
    height = top + row_h * len(runs) + 90
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
        f'font-family="Helvetica, Arial, sans-serif" font-size="13">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{left}" y="26" font-size="15" font-weight="bold">Flash use with two 1.5 MB OTA slots kept</text>',
    ]
    scale = plot_w / max_flash
    for row, run in enumerate(runs):
        y = top + row * row_h
        x = left
        info = run["installed"]
        for name, size, color in regions(run):
            w = size * scale
            if w > 0:
                parts.append(f'<rect x="{x:.1f}" y="{y}" width="{w:.1f}" height="28" fill="{color}" stroke="#667085" '
                             f'stroke-width="0.5"><title>{esc(name)}: {size / 1024:,.0f} KiB</title></rect>')
            x += w
        installed = [p["id"] for p in run["packs"] if p["status"] == "installed"]
        parts += [
            f'<text x="10" y="{y + 14}" font-weight="bold">{esc(run["label"])}</text>',
            f'<text x="10" y="{y + 30}" fill="#444">{info["flash_bytes"] // 1048576} MB flash</text>',
            f'<text x="{left}" y="{y + 46}" fill="#444">files free {info["fs_free"] / 1024:,.0f} KiB · packs: '
            f'{esc(", ".join(installed) or "none")} · OTA {"OK" if run["ota"]["ok"] and run["verify"]["ok"] else "FAILED"}</text>',
        ]
    ly = top + row_h * len(runs) + 10
    lx = left
    for mb in range(0, max_flash // 1048576 + 1):
        x = left + mb * 1048576 * scale
        parts.append(f'<text x="{x:.1f}" y="{ly}" text-anchor="middle" fill="#444">{mb} MB</text>')
    ly += 24
    for name, _, color in legend:
        item_w = 22 + 7.2 * len(name)
        if lx + item_w > width - right:
            lx, ly = left, ly + 20
        parts.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" fill="{color}" stroke="#667085" stroke-width="0.5"/>')
        parts.append(f'<text x="{lx + 16}" y="{ly + 11}">{esc(name)}</text>')
        lx += item_w
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def write_summary(runs, path: Path):
    pack_ids = [p["id"] for p in runs[0]["packs"]]
    pack_titles = {p["id"]: f"{p['title']} ({p['stored_bytes'] / 1024:,.0f} KiB)" for p in runs[0]["packs"]}
    lines = [
        "# Flash + OTA: what still fits after keeping two update slots",
        "",
        "- Same firmware source, ESP-IDF version and partition rule on every board: two 1.5 MB OTA slots, "
        "the rest of the flash as a FAT file area.",
        "- Resource packs are installed in order; a pack that does not fit is removed and the later packs are skipped.",
        "- After installing, the V2 firmware is sent by HTTP OTA; every stored file is then read back and hash-checked.",
        "",
        "| Board | Chip | Flash | Running firmware | OTA slot | File area | Files stored | Files free | "
        + " | ".join(pack_ids) + " | OTA V1→V2 | Files intact after OTA | Recorded | Note |",
        "|---|---|---:|---:|---:|---:|---:|---:|" + "---|" * len(pack_ids) + "---|---|---|---|",
    ]
    marks = {"installed": "yes", "does_not_fit": "does not fit", "skipped": "-"}
    for run in runs:
        info = run["installed"]
        slot = next(p["size"] for p in info["partitions"] if p["label"] == "ota_0")
        v = run["verify"]
        lines.append(
            f"| {run['label']} | {info['target']} | {info['flash_bytes'] // 1048576} MB "
            f"| {info['app_bytes'] / 1024:,.0f} KiB | {slot / 1024:,.0f} KiB ×2 | {info['fs_total'] / 1024:,.0f} KiB "
            f"| {(info['fs_total'] - info['fs_free']) / 1024:,.0f} KiB | {info['fs_free'] / 1024:,.0f} KiB | "
            + " | ".join(marks[p["status"]] for p in run["packs"])
            + f" | {'OK' if run['ota']['ok'] else 'failed'} ({run['ota']['seconds']} s) "
            f"| {v['files_checked'] - len(v['files_bad'])}/{v['files_checked']}, dashboard HTTP {v['dashboard_status']} "
            f"| {run['recorded_at']} | {run.get('note') or '-'} |")
    lines += ["", "Resource packs:", ""]
    lines += [f"- `{pid}`: {pack_titles[pid]} stored size" for pid in pack_ids]
    lines += ["", "![flash map](flash-map.svg)", ""]
    path.write_text("\n".join(lines))


def report(out_dir: Path):
    runs = [json.loads(p.read_text()) for p in sorted(out_dir.glob("*.json"))]
    if not runs:
        sys.exit(f"no board data in {out_dir}")
    draw_svg(runs, out_dir / "flash-map.svg")
    write_summary(runs, out_dir / "summary.md")
    for run in runs:
        installed = [p["id"] for p in run["packs"] if p["status"] == "installed"]
        print(f"{run['label']:<6} free {run['installed']['fs_free'] / 1024:7.0f} KiB  packs {', '.join(installed)}  "
              f"OTA {'OK' if run['ota']['ok'] and run['verify']['ok'] else 'FAILED'}")
    print(f"report: {out_dir / 'summary.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="label for this board in the comparison, e.g. C3")
    ap.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem* present)")
    ap.add_argument("--note", default="", help="free-text test conditions saved with this board's result")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="comparison folder that collects every board")
    ap.add_argument("--replot", action="store_true", help="only rebuild summary.md and flash-map.svg")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.replot:
        if not args.board:
            ap.error("give --board LABEL")
        run_board(args.board, args.port or find_port(), args.note, args.out)
    report(args.out)


if __name__ == "__main__":
    main()
