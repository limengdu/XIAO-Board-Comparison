#!/usr/bin/env python3
"""Run the classic CAN vs CAN FD bus test on one XIAO ESP32-C5 and add it to the comparison.

The board's two TWAI controllers each drive their own CAN transceiver on one real bus.
Node A sends the same data block to node B in three modes:
  classic   classic CAN, 8-byte frames,
  fd_nobrs  CAN FD 64-byte frames, whole frame at the arbitration rate,
  fd        CAN FD 64-byte frames, data phase switched to the data rate (BRS).
The board times each transfer and checks every byte. Results go to results/comparison/<label>.json;
summary.md and times.svg are rebuilt for every board recorded so far. C3, C6 and S3 appear in the
summary as capability rows, since their controllers handle classic CAN only.
Run inside an ESP-IDF shell (get_idf), which already provides pyserial.

Examples:
    python tools/run_canfd.py --board C5
    python tools/run_canfd.py --board C5-5M --data 5000000
    python tools/run_canfd.py --replot
"""

import argparse
import glob
import json
import statistics
import sys
import time
from datetime import datetime
from pathlib import Path

RESULTS_DIR = Path(__file__).resolve().parent.parent / "results"
DEFAULT_OUT = RESULTS_DIR / "comparison"
MODES = ("classic", "fd_nobrs", "fd")
MODE_NAMES = {
    "classic": "Classic CAN, 8-byte frames",
    "fd_nobrs": "CAN FD, 64-byte frames, no bit-rate switch",
    "fd": "CAN FD, 64-byte frames, faster data phase",
}
SHORT_NAMES = {"classic": "Classic, 8 B/frame", "fd_nobrs": "FD, 64 B/frame", "fd": "FD + BRS, 64 B/frame"}
# TWAI controllers per chip in ESP-IDF v6.1.1 (soc_caps.h); none of these three supports CAN FD.
# ESP-IDF v6.1.1 soc_caps.h 中各芯片的 TWAI 控制器数量；这三款都不支持 CAN FD。
OTHER_CHIPS = [("C3", "esp32c3", 1), ("C6", "esp32c6", 2), ("S3", "esp32s3", 1)]
WIRING_HINT = ("node A could not finish sending: no other node acknowledged the frames. "
               "Check 3V3/GND, TX/RX pins, CANH-CANH and CANL-CANL, and both termination switches.")


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
        self.ser.reset_input_buffer()
        self.pending = b""
        self.ser.write((line + "\r\n").encode())
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            self.pending += self.ser.read(65536)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                idx = text.find(tag + " {")
                if idx >= 0:
                    return json.loads(text[idx + len(tag) + 1:])
        raise TimeoutError(f"{self.ser.port}: no {tag} reply to '{line}' within {timeout_s:.0f}s")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"expected exactly one board on /dev/cu.usbmodem*, found {ports or 'none'}; pass --port")
    return ports[0]


def run_board(args, port):
    con = Console(port)
    try:
        info = con.command("info", "INFO", 5)
        print(f"[{args.board}] {info['target']}, {info['controllers']} TWAI controllers, "
              f"FD {'yes' if info['fd_supported'] else 'no'}")
        if not info["fd_supported"]:
            sys.exit("this firmware run needs a chip with CAN FD (ESP32-C5)")
        modes = {}
        for mode in MODES:
            modes[mode] = []
            for run in range(1, args.runs + 1):
                res = con.command(f"run mode={mode} bytes={args.bytes} arb={args.arb} data={args.data} run={run}",
                                  "RESULT", 60)
                if res.get("error") in ("tx_queue", "tx_stalled") and res.get("rx_unique", 0) == 0:
                    sys.exit(f"[{args.board}] {mode}: {WIRING_HINT}\n{json.dumps(res)}")
                if not res.get("ok"):
                    sys.exit(f"[{args.board}] {mode} run {run} failed:\n{json.dumps(res)}")
                modes[mode].append(res)
                print(f"  {mode:<8} run {run}: {res['elapsed_us'] / 1e6:6.3f} s, {res['frames']} frames, "
                      f"bus errors {res['node_a']['bus_err_num'] + res['node_b']['bus_err_num']}")
    finally:
        con.close()
    meta = {"label": args.board, "port": port, "note": args.note,
            "recorded_at": datetime.now().isoformat(timespec="seconds"), "info": info,
            "config": {"bytes": args.bytes, "arb_bps": args.arb, "data_bps": args.data, "runs": args.runs},
            "modes": modes}
    (args.out / f"{args.board}.json").write_text(json.dumps(meta, indent=2))


def mode_stats(run_list):
    times = [r["elapsed_us"] / 1e6 for r in run_list]
    med = statistics.median(times)
    first = run_list[0]
    return {
        "median": med, "min": min(times), "max": max(times), "n": len(times),
        "frames": first["frames"], "payload": first["payload"], "bytes": first["bytes"],
        "kbps": first["bytes"] * 8 / med / 1000,
        "all_match": all(r["match"] for r in run_list),
        "missing": sum(r["missing"] for r in run_list),
        "dup": sum(r["dup"] for r in run_list),
        "bus_errors": sum(r["node_a"]["bus_err_num"] + r["node_b"]["bus_err_num"] for r in run_list),
        "arb": first["arb_bps"], "data": first["data_bps"],
    }


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def draw_svg(runs, path: Path):
    """Horizontal bars: median time to move the data block per board and mode, with min-max whiskers."""
    width, left, right, top, bar_h, gap = 1000, 330, 120, 60, 20, 6
    colors = {"classic": "#98a2b3", "fd_nobrs": "#53b1fd", "fd": "#1f6fb2"}
    rows = [(run, m) for run in runs for m in MODES if m in run["modes"]]
    max_t = max(mode_stats(run["modes"][m])["max"] for run, m in rows)
    scale = (width - left - right) / max_t
    height = top + (bar_h + gap) * len(rows) + 14 * len(runs) + 30
    kib = runs[0]["config"]["bytes"] // 1024
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
        f'font-family="Helvetica, Arial, sans-serif" font-size="13">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="10" y="26" font-size="15" font-weight="bold">Time to move the same {kib} KiB over a real CAN bus '
        f'(median, lower is faster)</text>',
    ]
    y = top
    for run in runs:
        for mode in MODES:
            if mode not in run["modes"]:
                continue
            s = mode_stats(run["modes"][mode])
            rate = f"{s['arb'] // 1000} kbps" if mode != "fd" else f"{s['arb'] // 1000} k / {s['data'] / 1e6:g} Mbps"
            parts.append(f'<text x="10" y="{y + 14}"><tspan font-weight="bold">{esc(run["label"])}</tspan> '
                         f'{esc(SHORT_NAMES[mode])}, {rate}</text>')
            parts.append(f'<rect x="{left}" y="{y}" width="{s["median"] * scale:.1f}" height="{bar_h}" '
                         f'fill="{colors[mode]}"/>')
            parts.append(f'<line x1="{left + s["min"] * scale:.1f}" y1="{y + bar_h / 2}" '
                         f'x2="{left + s["max"] * scale:.1f}" y2="{y + bar_h / 2}" stroke="#101828"/>')
            parts.append(f'<text x="{left + s["max"] * scale + 6:.1f}" y="{y + 15}">{s["median"]:.3f} s</text>')
            y += bar_h + gap
        y += 14
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def write_summary(runs, path: Path):
    cfg = runs[0]["config"]
    lines = [
        "# CAN FD: classic CAN and CAN FD moving the same data block",
        "",
        f"- One XIAO ESP32-C5 drives two CAN transceivers on one twisted-pair bus with its two TWAI controllers; "
        f"node A sends a {cfg['bytes']:,}-byte block to node B, which acknowledges every frame.",
        "- Each frame carries its sequence number in a 29-bit extended ID, so the whole payload is data. "
        "Node B puts every frame in place by sequence number; the block is then compared by CRC32.",
        "- Time runs on the board from the first transmit to the last receive. Both bus terminations are 120 Ω.",
        "",
        "| Board | Mode | Arbitration | Data phase | Bytes/frame | Frames | Median time | Min–max | "
        "Payload rate | vs classic | Data intact | Missing | Bus errors | Runs | Recorded | Note |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---:|---:|---|---|",
    ]
    for run in runs:
        base = mode_stats(run["modes"]["classic"])["median"] if "classic" in run["modes"] else None
        for mode in MODES:
            if mode not in run["modes"]:
                continue
            s = mode_stats(run["modes"][mode])
            data = f"{s['data'] / 1e6:g} Mbps" if mode == "fd" else "same"
            ratio = f"{base / s['median']:.2f}×" if base else "-"
            lines.append(
                f"| {run['label']} | {MODE_NAMES[mode]} | {s['arb'] // 1000} kbps | {data} | {s['payload']} "
                f"| {s['frames']:,} | {s['median']:.3f} s | {s['min']:.3f}–{s['max']:.3f} s | {s['kbps']:.0f} kbps "
                f"| {ratio} | {'yes' if s['all_match'] else 'NO'} | {s['missing']} | {s['bus_errors']} | {s['n']} "
                f"| {run['recorded_at']} | {run.get('note') or '-'} |")
    lines += [
        "",
        "Other boards (controller capability, ESP-IDF v6.1.1 `soc_caps.h`):",
        "",
        "| Board | Chip | TWAI controllers | Classic CAN | CAN FD |",
        "|---|---|---:|---|---|",
        "| C5 | esp32c5 | 2 | yes | yes |",
    ]
    for label, chip, n in OTHER_CHIPS:
        lines.append(f"| {label} | {chip} | {n} | yes | not supported |")
    lines += [
        "",
        "A 64-byte frame carries 8× the data of an 8-byte frame, but the ID, control field, CRC and gaps "
        "are paid per frame, so the gain in time is the ratio in the *vs classic* column.",
        "",
        "![transfer times](times.svg)",
        "",
    ]
    path.write_text("\n".join(lines))


def report(out_dir: Path):
    runs = sorted((json.loads(p.read_text()) for p in out_dir.glob("*.json")), key=lambda r: r["recorded_at"])
    if not runs:
        sys.exit(f"no board data in {out_dir}")
    draw_svg(runs, out_dir / "times.svg")
    write_summary(runs, out_dir / "summary.md")
    for run in runs:
        for mode in MODES:
            if mode in run["modes"]:
                s = mode_stats(run["modes"][mode])
                print(f"{run['label']:<8} {mode:<8} {s['median']:6.3f} s  {s['kbps']:6.0f} kbps  "
                      f"intact {'yes' if s['all_match'] else 'NO'}")
    print(f"report: {out_dir / 'summary.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="label for this run in the comparison, e.g. C5")
    ap.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem* present)")
    ap.add_argument("--bytes", type=int, default=65536, help="data block size, multiple of 64 (default 65536)")
    ap.add_argument("--arb", type=int, default=500000, help="arbitration bitrate (default 500000)")
    ap.add_argument("--data", type=int, default=2000000, help="CAN FD data-phase bitrate (default 2000000)")
    ap.add_argument("--runs", type=int, default=3, help="runs per mode (default 3)")
    ap.add_argument("--note", default="", help="free-text test conditions saved with this result")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="comparison folder that collects every run")
    ap.add_argument("--replot", action="store_true", help="only rebuild summary.md and times.svg")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.replot:
        if not args.board:
            ap.error("give --board LABEL")
        if args.bytes % 64:
            ap.error("--bytes must be a multiple of 64")
        run_board(args, args.port or find_port())
    report(args.out)


if __name__ == "__main__":
    main()
