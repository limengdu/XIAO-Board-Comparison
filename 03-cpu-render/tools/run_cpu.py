#!/usr/bin/env python3
"""Run the CPU render benchmark on one XIAO board and add it to the comparison.

The board renders the same four fractal images with integer-only math, timed on the board:
  seq  one task renders the four images one after another,
  par  four tasks render one image each at the same time (spread over both cores on the S3).
Each mode runs several times. The script then pulls the four images (rendered again, untimed),
saves them as <label>.png and checks that the pixels match the timed runs. Results go to
results/comparison/<label>.json; summary.md and times.svg are rebuilt for every board recorded so
far. Run inside an ESP-IDF shell (get_idf), which already provides pyserial.

Examples:
    python tools/run_cpu.py --board C3
    python tools/run_cpu.py --board S3 --port /dev/cu.usbmodem2101 --runs 7
    python tools/run_cpu.py --replot
"""

import argparse
import base64
import colorsys
import glob
import json
import statistics
import struct
import sys
import time
import zlib
from datetime import datetime
from pathlib import Path

RESULTS_DIR = Path(__file__).resolve().parent.parent / "results"
DEFAULT_OUT = RESULTS_DIR / "comparison"
MODES = ("seq", "par")


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
        idx = text.find(tag + " {")
        return json.loads(text[idx + len(tag) + 1:]) if idx >= 0 else None

    def command(self, line: str, tag: str, timeout_s: float):
        self.send(line)
        for text in self.lines(timeout_s):
            obj = self.tagged(text, tag)
            if obj is not None:
                return obj

    def bench(self, mode: str, runs: int, timeout_s: float):
        self.send(f"bench mode={mode} runs={runs}")
        results = []
        for text in self.lines(timeout_s):
            res = self.tagged(text, "RESULT")
            if res is not None:
                if not res.get("ok"):
                    raise RuntimeError(f"bench failed: {res}")
                results.append(res)
                print(f"  {mode} run {res['run']}: {res['wall_us'] / 1e6:6.3f} s")
            elif self.tagged(text, "DONE") is not None:
                return results

    def dump(self, scene: int, timeout_s: float):
        """Return (header, pixel bytes, board CRC) for one image."""
        self.send(f"dump {scene}")
        header, rows = None, []
        for text in self.lines(timeout_s):
            if header is None:
                header = self.tagged(text, "IMG_BEGIN")
            elif text.startswith("R "):
                rows.append(base64.b64decode(text[2:]))
            else:
                end = self.tagged(text, "IMG_END")
                if end is None:
                    continue
                pixels = b"".join(rows)
                if not end.get("ok") or len(pixels) != header["width"] * header["height"] \
                        or zlib.crc32(pixels) != end["crc32"]:
                    raise RuntimeError(f"image {scene} transfer check failed: {end}")
                return header, pixels, end["crc32"]


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"expected exactly one board on /dev/cu.usbmodem*, found {ports or 'none'}; pass --port")
    return ports[0]


def palette():
    """RGB for each iteration count; points that never escape are black."""
    colors = []
    for n in range(256):
        if n == 255:
            colors.append((0, 0, 0))
            continue
        t = (n % 64) / 64
        r, g, b = colorsys.hsv_to_rgb(0.62 - 0.55 * t, 0.85, min(1.0, 0.25 + 3 * t))
        colors.append((int(r * 255), int(g * 255), int(b * 255)))
    return colors


def write_png(path: Path, images, width, height, gap=4):
    """Save the four images as a 2x2 grid PNG."""
    pal = palette()
    w, h = width * 2 + gap, height * 2 + gap
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        row_img, iy = divmod(y, height + gap)
        for x in range(w):
            col_img, ix = divmod(x, width + gap)
            if iy >= height or ix >= width:
                raw += b"\xff\xff\xff"
            else:
                raw += bytes(pal[images[row_img * 2 + col_img][iy * width + ix]])

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b"")
    path.write_bytes(png)


def run_board(label, port, runs, note, out_dir: Path):
    con = Console(port)
    try:
        info = con.command("info", "INFO", 5)
        print(f"[{label}] {info['target']}, {info['cores']} core(s), {info['cpu_mhz']} MHz")
        modes = {}
        for mode in MODES:
            modes[mode] = con.bench(mode, runs, timeout_s=runs * 30 + 30)
        images, crcs = [], []
        for scene in range(len(info["scenes"])):
            header, pixels, crc = con.dump(scene, timeout_s=60)
            images.append(pixels)
            crcs.append(crc)
    finally:
        con.close()
    timed = {(img["scene"], img["crc32"]) for runs_ in modes.values() for r in runs_ for img in r["images"]}
    consistent = timed == set(zip(info["scenes"], crcs))
    write_png(out_dir / f"{label}.png", images, info["width"], info["height"])
    meta = {"label": label, "port": port, "note": note, "recorded_at": datetime.now().isoformat(timespec="seconds"),
            "info": info, "modes": modes, "image_crc32": crcs, "pixels_match_timed_runs": consistent}
    (out_dir / f"{label}.json").write_text(json.dumps(meta, indent=2))
    print(f"[{label}] images saved to {label}.png, pixels match timed runs: {consistent}")


def stats(run_list):
    walls = [r["wall_us"] / 1e6 for r in run_list]
    return {"median": statistics.median(walls), "min": min(walls), "max": max(walls), "n": len(walls)}


def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def draw_svg(runs, path: Path):
    """Horizontal bars: median wall time per board for seq and par, with min-max whiskers."""
    width, left, right, top, bar_h, group_h = 1000, 170, 120, 60, 18, 56
    colors = {"seq": "#1f6fb2", "par": "#f79009"}
    names = {"seq": "One after another (one task)", "par": "Four at once (four tasks)"}
    max_t = max(stats(r["modes"][m])["max"] for r in runs for m in MODES)
    scale = (width - left - right) / max_t
    height = top + group_h * len(runs) + 60
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
        f'font-family="Helvetica, Arial, sans-serif" font-size="13">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{left}" y="26" font-size="15" font-weight="bold">Time to render the same four images '
        f'(median, lower is faster)</text>',
    ]
    for row, run in enumerate(runs):
        y = top + row * group_h
        info = run["info"]
        parts += [
            f'<text x="10" y="{y + 14}" font-weight="bold">{esc(run["label"])}</text>',
            f'<text x="10" y="{y + 31}" fill="#444">{info["cpu_mhz"]} MHz, {info["cores"]} core'
            f'{"s" if info["cores"] > 1 else ""}</text>',
        ]
        for i, mode in enumerate(MODES):
            s = stats(run["modes"][mode])
            by = y + i * (bar_h + 2)
            parts.append(f'<rect x="{left}" y="{by}" width="{s["median"] * scale:.1f}" height="{bar_h}" '
                         f'fill="{colors[mode]}"/>')
            parts.append(f'<line x1="{left + s["min"] * scale:.1f}" y1="{by + bar_h / 2}" '
                         f'x2="{left + s["max"] * scale:.1f}" y2="{by + bar_h / 2}" stroke="#101828"/>')
            parts.append(f'<text x="{left + s["max"] * scale + 6:.1f}" y="{by + 13}">{s["median"]:.2f} s</text>')
    ly = top + group_h * len(runs) + 10
    lx = left
    for mode in MODES:
        parts.append(f'<rect x="{lx}" y="{ly}" width="12" height="12" fill="{colors[mode]}"/>')
        parts.append(f'<text x="{lx + 16}" y="{ly + 11}">{esc(names[mode])}</text>')
        lx += 260
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def write_summary(runs, path: Path):
    base = next((r for r in runs if r["label"] == "C3"), runs[0])
    base_seq = stats(base["modes"]["seq"])["median"]
    reference = runs[0]["image_crc32"]
    info0 = runs[0]["info"]
    iters = runs[0]["modes"]["seq"][0]["iters"]
    lines = [
        "# CPU: time to render the same four images",
        "",
        f"- Four fractal images ({', '.join(info0['scenes'])}), {info0['width']}×{info0['height']} each, "
        f"up to {info0['max_iter']} iterations per pixel, {iters:,} iterations in total.",
        "- Integer-only fixed-point math (Q7.24), so every chip computes exactly the same pixels; "
        "same source, ESP-IDF version and -O2 optimisation; Wi-Fi off.",
        "- `seq`: one task renders the four images one after another. "
        "`par`: four tasks render one image each at the same time; on the dual-core S3 they spread over both cores.",
        "- Only computation is timed on the board with esp_timer; images are sent to the computer afterwards.",
        "",
        f"| Board | Chip | Cores | CPU MHz | seq median | seq min–max | par median | par min–max | "
        f"Speed vs {base['label']} (seq) | Runs | Same pixels as {runs[0]['label']} | Recorded | Note |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|---|---|",
    ]
    for run in runs:
        info = run["info"]
        s, p = stats(run["modes"]["seq"]), stats(run["modes"]["par"])
        same = run["image_crc32"] == reference and run["pixels_match_timed_runs"]
        lines.append(
            f"| {run['label']} | {info['target']} | {info['cores']} | {info['cpu_mhz']} "
            f"| {s['median']:.3f} s | {s['min']:.3f}–{s['max']:.3f} s | {p['median']:.3f} s "
            f"| {p['min']:.3f}–{p['max']:.3f} s | {base_seq / s['median']:.2f}× | {s['n']}+{p['n']} "
            f"| {'yes' if same else 'NO'} | {run['recorded_at']} | {run.get('note') or '-'} |")
    lines += ["", "![render times](times.svg)", "", f"![four images]({runs[0]['label']}.png)", ""]
    path.write_text("\n".join(lines))


def report(out_dir: Path):
    runs = [json.loads(p.read_text()) for p in sorted(out_dir.glob("*.json"))]
    if not runs:
        sys.exit(f"no board data in {out_dir}")
    draw_svg(runs, out_dir / "times.svg")
    write_summary(runs, out_dir / "summary.md")
    for run in runs:
        s, p = stats(run["modes"]["seq"]), stats(run["modes"]["par"])
        print(f"{run['label']:<6} seq {s['median']:6.3f} s   par {p['median']:6.3f} s")
    print(f"report: {out_dir / 'summary.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="label for this board in the comparison, e.g. C3")
    ap.add_argument("--port", help="serial port (default: the only /dev/cu.usbmodem* present)")
    ap.add_argument("--runs", type=int, default=5, help="runs per mode (default 5)")
    ap.add_argument("--note", default="", help="free-text test conditions saved with this board's result")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="comparison folder that collects every board")
    ap.add_argument("--replot", action="store_true", help="only rebuild summary.md and times.svg")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.replot:
        if not args.board:
            ap.error("give --board LABEL")
        run_board(args.board, args.port or find_port(), args.runs, args.note, args.out)
    report(args.out)


if __name__ == "__main__":
    main()
