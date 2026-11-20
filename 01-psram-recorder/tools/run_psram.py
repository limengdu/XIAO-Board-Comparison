#!/usr/bin/env python3
"""Run the PSRAM history-recorder experiment on one XIAO board and add it to the comparison.

The board records simulated 3-axis data in real time; the anomaly is at 10 s. After
recording, the script pulls the ring buffer, checks the CRC, saves it under the board's
label, and redraws the summary and SVG for every board recorded so far. Re-running a
label replaces that board's result. Run inside an ESP-IDF shell (get_idf), which already
provides pyserial.

Examples:
    python tools/run_psram.py --board C3
    python tools/run_psram.py --board C5-internal --mem internal
    python tools/run_psram.py --replot
"""

import argparse
import base64
import glob
import json
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

RESULTS_DIR = Path(__file__).resolve().parent.parent / "results"
DEFAULT_OUT = RESULTS_DIR / "comparison"
SPIKE_AT_S = 10
SPIKE_THRESHOLD = 15000


class RecorderConsole:
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

    def send(self, line: str):
        self.ser.reset_input_buffer()
        self.pending = b""
        self.ser.write((line + "\r\n").encode())

    def lines(self, timeout_s: float):
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            self.pending += self.ser.read(65536)
            while b"\n" in self.pending:
                line, self.pending = self.pending.split(b"\n", 1)
                yield line.decode(errors="replace").strip()
        raise TimeoutError(f"{self.ser.port}: no reply within {timeout_s:.0f}s")

    @staticmethod
    def tagged(text: str, tag: str):
        """Return the JSON object of a "<tag> {...}" line, or None."""
        idx = text.find(tag + " {")
        return json.loads(text[idx + len(tag) + 1:]) if idx >= 0 else None

    def command(self, line: str, tag: str, timeout_s: float, on_line=None):
        self.send(line)
        for text in self.lines(timeout_s):
            obj = self.tagged(text, tag)
            if obj is not None:
                return obj
            if on_line:
                on_line(text)

    def dump(self, timeout_s: float):
        """Return (header, raw bytes) after checking the length and CRC32."""
        self.send("dump")
        header, chunks = None, []
        for text in self.lines(timeout_s):
            if header is None:
                header = self.tagged(text, "DUMP_BEGIN")
                end = self.tagged(text, "DUMP_END")
                if end is not None:
                    raise RuntimeError(f"dump failed: {end}")
            elif text.startswith("D "):
                chunks.append(base64.b64decode(text[2:]))
            else:
                end = self.tagged(text, "DUMP_END")
                if end is None:
                    continue
                raw = b"".join(chunks)
                if not end.get("ok") or len(raw) != end["bytes"] or zlib.crc32(raw) != end["crc32"]:
                    raise RuntimeError(f"dump check failed: got {len(raw)} bytes crc {zlib.crc32(raw)}, board {end}")
                return header, raw


def find_port():
    """Return the only connected USB serial port."""
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"expected exactly one board on /dev/cu.usbmodem*, found {ports or 'none'}; pass --port")
    return ports[0]


def run_board(label, port, mem, seconds, note, out_dir: Path):
    """Record on one board, then save <label>.bin and <label>.json into the comparison folder."""
    con = RecorderConsole(port)
    try:
        info = con.command("info", "INFO", 5)
        start = con.command(f"record seconds={seconds} mem={mem}", "START", 10)
        print(f"[{label}] {info['target']} recording into {start['memory']}, "
              f"buffer {start['buffer_bytes']:,} of {start['wanted_bytes']:,} bytes "
              f"({start['capacity_s']:.1f} s)")
        result = None
        for text in con.lines(seconds + 30):
            obj = RecorderConsole.tagged(text, "PROGRESS")
            if obj:
                print(f"[{label}] {obj['t_s']:>3} s  retained {obj['retained'] / 2000:.1f} s")
            result = RecorderConsole.tagged(text, "RESULT")
            if result is not None:
                break
        if not result.get("ok"):
            sys.exit(f"[{label}] record failed: {result}")
        t0 = time.monotonic()
        header, raw = con.dump(120)
        dump_s = time.monotonic() - t0
    finally:
        con.close()
    (out_dir / f"{label}.bin").write_bytes(raw)
    meta = {"label": label, "port": port, "mem": mem, "seconds": seconds, "note": note,
            "recorded_at": datetime.now().isoformat(timespec="seconds"), "info": info,
            "start": start, "result": result, "dump": header, "dump_seconds": round(dump_s, 2)}
    (out_dir / f"{label}.json").write_text(json.dumps(meta, indent=2))
    print(f"[{label}] saved {len(raw):,} bytes in {dump_s:.1f} s, CRC OK")


def load_runs(out_dir: Path):
    runs = []
    for meta_path in sorted(out_dir.glob("*.json")):
        meta = json.loads(meta_path.read_text())
        raw = (out_dir / f"{meta['label']}.bin").read_bytes()
        meta["xyz"] = list(struct.iter_unpack("<hhh", raw))
        runs.append(meta)
    return runs


def analyse(run):
    """Retained time range and whether the 10 s anomaly is still in the buffer, from the data itself."""
    rate = run["dump"]["sample_rate"]
    first = run["dump"]["first_sample"]
    hits = [i for i, (x, _, _) in enumerate(run["xyz"]) if abs(x) >= SPIKE_THRESHOLD]
    return {
        "from_s": first / rate,
        "to_s": (first + len(run["xyz"])) / rate,
        "spike_found": bool(hits),
        "spike_at_s": (first + hits[0]) / rate if hits else None,
    }


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def draw_svg(runs, seconds, path: Path):
    """Stack one X-axis trace per board on a shared 0..seconds axis (min/max envelope per pixel)."""
    width, left, right, row_h, top = 1100, 210, 30, 120, 50
    plot_w = width - left - right
    height = top + row_h * len(runs) + 40
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}" font-family="Helvetica, Arial, sans-serif" font-size="13">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{left}" y="24" font-size="15" font-weight="bold">'
        f'X axis history kept on each board after {seconds} s of 2 kHz recording</text>',
    ]
    sx = lambda t: left + plot_w * t / seconds  # noqa: E731
    for row, run in enumerate(runs):
        a = analyse(run)
        rate = run["dump"]["sample_rate"]
        first = run["dump"]["first_sample"]
        y0 = top + row * row_h
        mid = y0 + row_h / 2 - 8
        half = row_h / 2 - 18
        parts.append(f'<rect x="{left}" y="{y0}" width="{plot_w}" height="{row_h - 16}" fill="#f4f4f4"/>')
        if a["from_s"] > 0:
            parts.append(f'<rect x="{left}" y="{y0}" width="{sx(a["from_s"]) - left:.1f}" height="{row_h - 16}" '
                         f'fill="#e0e0e0"/>')
            parts.append(f'<text x="{(left + sx(a["from_s"])) / 2:.1f}" y="{mid + 4:.1f}" text-anchor="middle" '
                         f'fill="#777">overwritten</text>')
        cols = {}
        for i, (x, _, _) in enumerate(run["xyz"]):
            c = int(plot_w * (first + i) / rate / seconds)
            lo, hi = cols.get(c, (x, x))
            cols[c] = (min(lo, x), max(hi, x))
        path_d = []
        for c in sorted(cols):
            lo, hi = cols[c]
            ylo = mid - half * max(-1.0, min(1.0, lo / 20000))
            yhi = mid - half * max(-1.0, min(1.0, hi / 20000))
            path_d.append(f"M{left + c + 0.5:.1f} {ylo:.1f}V{yhi:.1f}")
        parts.append(f'<path d="{" ".join(path_d)}" stroke="#1f6fb2" stroke-width="1" fill="none"/>')
        info = run["info"]
        mem = run["start"]["memory"]
        status = (f'anomaly kept ({a["spike_at_s"]:.2f} s)' if a["spike_found"] else "anomaly lost")
        color = "#1a7f37" if a["spike_found"] else "#c62828"
        parts += [
            f'<text x="10" y="{y0 + 20}" font-weight="bold">{esc(run["label"])}</text>',
            f'<text x="10" y="{y0 + 38}" fill="#444">{esc(info["target"])}, {esc(mem)} buffer</text>',
            f'<text x="10" y="{y0 + 56}" fill="#444">{run["start"]["buffer_bytes"] / 1024:,.0f} KiB, '
            f'keeps {a["to_s"] - a["from_s"]:.1f} s</text>',
            f'<text x="10" y="{y0 + 74}" fill="{color}" font-weight="bold">{status}</text>',
        ]
    marker = sx(SPIKE_AT_S)
    bottom = top + row_h * len(runs) - 16
    parts.append(f'<line x1="{marker:.1f}" y1="{top - 8}" x2="{marker:.1f}" y2="{bottom}" stroke="#d32f2f" '
                 f'stroke-dasharray="4 3"/>')
    parts.append(f'<text x="{marker:.1f}" y="{top - 12}" text-anchor="middle" fill="#d32f2f">anomaly at '
                 f'{SPIKE_AT_S} s</text>')
    for t in range(0, seconds + 1, 10):
        parts.append(f'<text x="{sx(t):.1f}" y="{bottom + 20}" text-anchor="middle" fill="#444">{t} s</text>')
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def write_summary(runs, seconds, path: Path):
    lines = [
        f"# PSRAM history recorder, {seconds} s at 2 kHz",
        "",
        f"- Simulated 3-axis data, 6 bytes per sample; anomaly injected at {SPIKE_AT_S} s.",
        f"- Full history needs {seconds * 2000 * 6:,} bytes.",
        "- Boards are recorded one at a time with the same firmware source and ESP-IDF version.",
        "",
        "| Board | Chip | CPU MHz | Buffer memory | Buffer bytes | History kept | Range kept | Anomaly | "
        "Internal free after | Dump time | Recorded | Note |",
        "|---|---|---:|---|---:|---:|---|---|---:|---:|---|---|",
    ]
    for run in runs:
        a = analyse(run)
        r = run["result"]
        info = run["info"]
        lines.append(
            f"| {run['label']} | {info['target']} v{info['revision'] // 100}.{info['revision'] % 100} "
            f"| {info['cpu_mhz']} | {r['memory']} "
            f"| {r['buffer_bytes']:,} | {a['to_s'] - a['from_s']:.1f} s | {a['from_s']:.1f}–{a['to_s']:.1f} s "
            f"| {'kept' if a['spike_found'] else 'lost'} | {r['internal_free_after']:,} | {run['dump_seconds']} s "
            f"| {run.get('recorded_at', '-')} | {run.get('note') or '-'} |")
    lines += ["", "![waveforms](waveforms.svg)", ""]
    path.write_text("\n".join(lines))


def report(out_dir: Path):
    runs = load_runs(out_dir)
    if not runs:
        sys.exit(f"no board data in {out_dir}")
    lengths = {run["seconds"] for run in runs}
    if len(lengths) != 1:
        sys.exit(f"boards in {out_dir} used different recording lengths: {sorted(lengths)}")
    seconds = lengths.pop()
    draw_svg(runs, seconds, out_dir / "waveforms.svg")
    write_summary(runs, seconds, out_dir / "summary.md")
    for run in runs:
        a = analyse(run)
        print(f"{run['label']:<14} kept {a['from_s']:5.1f}–{a['to_s']:5.1f} s  "
              f"anomaly {'kept' if a['spike_found'] else 'LOST'}")
    print(f"report: {out_dir / 'summary.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="label for this board in the comparison, e.g. C3, C5, C5-internal")
    ap.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem* present)")
    ap.add_argument("--mem", choices=("auto", "internal"), default="auto",
                    help="auto uses PSRAM when present; internal forces internal RAM (default auto)")
    ap.add_argument("--seconds", type=int, default=60, help="recording length (default 60)")
    ap.add_argument("--note", default="", help="free-text test conditions saved with this board's result")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT,
                    help=f"comparison folder that collects every board (default {DEFAULT_OUT.relative_to(RESULTS_DIR.parent)})")
    ap.add_argument("--replot", action="store_true", help="only rebuild summary.md and waveforms.svg")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.replot:
        if not args.board:
            ap.error("give --board LABEL")
        run_board(args.board, args.port or find_port(), args.mem, args.seconds, args.note, args.out)
    report(args.out)


if __name__ == "__main__":
    main()
