#!/usr/bin/env python3
"""Measure Wi-Fi idle current with power save and Wi-Fi 6 Target Wake Time on one XIAO board.

The board runs the same light network task in every phase: it stays connected, answers a PING
from this computer every 3-7 s and sends a 64-byte report every 10 s. Only the power-save method
changes between phases:
  ps          802.11 modem sleep, waking for every DTIM beacon (Wi-Fi 4/5/6),
  twt<ms>     individual TWT agreement with the router, waking every <ms> ms (Wi-Fi 6 only),
  twt<ms>w<n> the same with an <n> ms wake window instead of the ESP-IDF example's 65 ms.

Steps:
  1. With USB connected, the script writes the Wi-Fi settings from ../.env, this computer's
     address and the plan into the board, and arms it to run on its own at the next power-up.
  2. You unplug USB. The PPK2 powers the board at the battery pads (3.7 V by default) and
     records current at 100 kS/s.
  3. The board reports BEGIN/END of each phase over UDP; the script lines these markers up with
     the current trace and computes the mean current and ping round-trip time per phase.
     After the last phase the board enters deep sleep, which gives the sleep floor as well.

Results go to results/comparison/<label>.json; summary.md, current.svg and traces.svg are rebuilt
for every board recorded so far. Run with the project venv, which holds ppk2-api and pyserial.

Examples:
    .venv/bin/python tools/run_power.py --board C5
    .venv/bin/python tools/run_power.py --board C5 --dry-run --secs 20
    .venv/bin/python tools/run_power.py --replot
"""

import argparse
import glob
import json
import random
import socket
import statistics
import sys
import threading
import time
from datetime import datetime
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ENV_FILE = ROOT.parent / ".env"
DEFAULT_OUT = ROOT / "results" / "comparison"
CMD_PORT = 5005
HOST_PORT = 5006
SAMPLE_HZ = 100_000
BIN_SAMPLES = 100                 # 1 ms bins / 1 ms 一格
PING_GAP_S = (3.0, 7.0)
EDGE_BEGIN_S = 1.0                # trimmed after BEGIN / BEGIN 后裁掉的时间
EDGE_END_S = 0.5                  # trimmed before END / END 前裁掉的时间
TRACE_S = 3.0
HELLO_TIMEOUT_S = 45              # board must report on Wi-Fi within this time / 板子须在此时间内通过 Wi-Fi 报到
SLEEP_WINDOW_S = (1.5, 4.5)       # deep-sleep window after DONE / DONE 之后的深睡统计区间
DEFAULT_PLANS = {
    "esp32c5": "2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8,5g/ps,5g/twt500,5g/twt2000,5g/twt2000w8",
    "esp32c6": "2g/ps,2g/twt500,2g/twt2000,2g/twt2000w8",
    "esp32c3": "2g/ps",
    "esp32s3": "2g/ps",
}


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
        self.ser.write((line + "\r\n").encode())

    def command(self, line: str, tag: str, timeout_s: float):
        self.ser.reset_input_buffer()
        self.pending = b""
        self.send(line)
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            self.pending += self.ser.read(65536)
            while b"\n" in self.pending:
                raw, self.pending = self.pending.split(b"\n", 1)
                text = raw.decode(errors="replace").strip()
                idx = text.find(tag + " {")
                if idx >= 0:
                    return json.loads(text[idx + len(tag) + 1:])
        raise TimeoutError(f"{self.ser.port}: no {tag} reply within {timeout_s:.0f}s")


def load_env(path: Path):
    env = {}
    for line in path.read_text().splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, val = line.split("=", 1)
            env[key.strip()] = val.strip()
    for key in ("WIFI_SSID", "WIFI_PASSWORD"):
        if not env.get(key):
            sys.exit(f"{path}: {key} is empty")
    return env


def local_ip():
    """IPv4 address of the physical interface that carries the default route (macOS)."""
    import subprocess

    route = subprocess.run(["route", "-n", "get", "default"], capture_output=True, text=True).stdout
    iface = next((l.split(":", 1)[1].strip() for l in route.splitlines() if l.strip().startswith("interface:")), "")
    if iface and not iface.startswith("utun"):
        addr = subprocess.run(["ipconfig", "getifaddr", iface], capture_output=True, text=True).stdout.strip()
        if addr:
            return addr
    sys.exit("could not find this computer's LAN address; pass --host")


def find_port():
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if len(ports) != 1:
        sys.exit(f"expected exactly one board on /dev/cu.usbmodem*, found {ports or 'none'}; pass --port")
    return ports[0]


# ---------------------------------------------------------------------------- PPK2

class Recorder(threading.Thread):
    """Store the raw PPK2 stream during the run; decode() turns it into 1 ms mean-current bins (µA).
    Sample n is placed at t0 + n / SAMPLE_HZ on the host monotonic time base."""

    def __init__(self, ppk):
        super().__init__(daemon=True)
        self.ppk = ppk
        self.chunks = []
        self.bins = []
        self.t0 = None             # host time of sample 0 / 第 0 个采样点对应的主机时间
        self.stop_evt = threading.Event()
        self.error = None

    def run(self):
        self.ppk.ser.timeout = 0.02
        while not self.stop_evt.is_set():
            try:
                raw = self.ppk.ser.read(65536)
            except OSError as e:
                self.error = f"PPK2 disconnected ({e}); connect its POWER ONLY port to a USB charger"
                return
            if raw:
                if self.t0 is None:
                    self.t0 = time.monotonic() - len(raw) / 4 / SAMPLE_HZ
                self.chunks.append(raw)

    def decode(self):
        """Convert every stored sample with the PPK2 calibration and average into 1 ms bins.
        用 PPK2 校准参数换算全部采样点，并按 1 ms 求平均。"""
        import struct

        m = self.ppk.modifiers
        vdd = self.ppk.current_vdd / 1000
        mult = self.ppk.adc_mult
        coef = []
        for r in range(5):
            k = str(r)
            coef.append((m["O"][k], mult / m["R"][k], m["UG"][k], m["GS"][k], m["GI"][k],
                         m["S"][k] * vdd + m["I"][k]))
        data = b"".join(self.chunks)
        data = data[:len(data) // 4 * 4]
        bins, acc, n = [], 0.0, 0
        for (word,) in struct.iter_unpack("<I", data):
            o, k, ug, gs, gi, off = coef[min((word >> 14) & 7, 4)]
            x = ((word & 0x3FFF) * 4 - o) * k
            acc += ug * (x * (gs * x + gi) + off)
            n += 1
            if n == BIN_SAMPLES:
                bins.append(acc / BIN_SAMPLES * 1e6)
                acc, n = 0.0, 0
        self.bins = bins
        self.chunks = []

    def window(self, t_from: float, t_to: float):
        if self.t0 is None:
            return []
        a = max(0, int((t_from - self.t0) * 1000))
        b = min(len(self.bins), int((t_to - self.t0) * 1000))
        return self.bins[a:b] if b > a else []


def open_ppk(port, mv):
    from ppk2_api.ppk2_api import PPK2_API

    # The PPK2 exposes two serial ports; use the one that returns calibration metadata.
    # PPK2 提供两个串口，选用能返回校准数据的那一个。
    candidates = [port] if port else PPK2_API.list_devices()
    if not candidates:
        sys.exit("no PPK2 found; connect its USB DATA/POWER port to this computer")
    ppk = None
    for dev in candidates:
        ppk = PPK2_API(dev, timeout=1, write_timeout=1, exclusive=True)
        ok = False
        for _ in range(3):
            ppk.stop_measuring()
            quiet_since = time.monotonic()
            while time.monotonic() - quiet_since < 0.5:
                if ppk.ser.read(65536):
                    quiet_since = time.monotonic()
            try:
                ok = bool(ppk.get_modifiers())
            except UnicodeDecodeError:
                ok = False
            if ok:
                break
        if ok:
            break
        ppk.ser.close()
        ppk = None
    if ppk is None:
        sys.exit(f"PPK2 did not answer on {candidates}")
    ppk.use_source_meter()
    ppk.set_source_voltage(mv)
    ppk.toggle_DUT_power("OFF")
    return ppk


# ---------------------------------------------------------------------------- run

def configure(args, env):
    con = Console(args.port)
    try:
        info = con.command("info", "INFO", 5)
        plan = args.plan or DEFAULT_PLANS.get(info["target"], "2g/ps")
        if "5g/" in plan and not info.get("band5"):
            sys.exit(f"{info['target']} has no 5 GHz radio; drop the 5g phases from --plan")
        if "twt" in plan and not info.get("wifi6"):
            sys.exit(f"{info['target']} has no Wi-Fi 6; use ps phases only")
        ssid5 = env.get("WIFI_SSID_5G") or env["WIFI_SSID"]
        res = con.command(f"config ssid={env['WIFI_SSID']} ssid5={ssid5} pass={env['WIFI_PASSWORD']} "
                          f"host={args.host} plan={plan} secs={args.secs} autorun={0 if args.dry_run else 1}",
                          "RESULT", 5)
        if not res.get("ok"):
            sys.exit(f"config rejected: {res}")
        info = con.command("info", "INFO", 5)
    except BaseException:
        con.close()
        raise
    info["plan"], info["secs"] = plan, args.secs
    (args.out.parent / f".configured-{args.board}.json").write_text(json.dumps(info))
    print(f"[{args.board}] {info['target']} {info['mac']}, plan {plan}, {args.secs} s per phase, "
          f"markers to {args.host}:{HOST_PORT}")
    return con, info, plan


def collect(args, n_phases, recorder=None):
    """Receive the board's markers, ping it during each phase and return the event log."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("", HOST_PORT))
    sock.settimeout(0.05)
    events, pings = [], {}
    board, in_phase, seq = None, None, 0
    next_ping = 0.0
    t_start = time.monotonic()
    deadline = t_start + 180 + n_phases * (args.secs + 30)
    t_done = None
    try:
        while time.monotonic() < deadline:
            now = time.monotonic()
            if t_done is not None and now > t_done + SLEEP_WINDOW_S[1] + 0.5:
                break
            if recorder and recorder.error:
                sys.exit(recorder.error)
            if board is None and now > t_start + HELLO_TIMEOUT_S:
                print(f"  no HELLO within {HELLO_TIMEOUT_S} s; stopping", flush=True)
                break
            if board and in_phase is not None and now >= next_ping:
                seq += 1
                pings[seq] = {"phase": in_phase, "sent": now, "rtt_ms": None}
                try:
                    sock.sendto(f"PING {seq}".encode(), (board, CMD_PORT))
                except OSError:
                    pass        # counted as lost / 计为丢失
                next_ping = now + random.uniform(*PING_GAP_S)
            try:
                data, addr = sock.recvfrom(1024)
            except socket.timeout:
                continue
            now = time.monotonic()
            text = data.decode(errors="replace").strip()
            tag, _, body = text.partition(" ")
            if tag == "PONG":
                p = pings.get(int(body) if body.isdigit() else -1)
                if p and p["rtt_ms"] is None:
                    p["rtt_ms"] = (now - p["sent"]) * 1000
                continue
            try:
                obj = json.loads(body)
            except json.JSONDecodeError:
                continue
            # The board sends every marker several times; keep the first copy.
            # 板子每条标记会发送多次，只保留第一份。
            if tag in ("BEGIN", "END", "DONE") and any(
                    e["tag"] == tag and e.get("idx") == obj.get("idx") for e in events):
                continue
            if tag == "HELLO" and events and events[-1]["tag"] == "HELLO":
                continue
            events.append({"t": now, "tag": tag, **obj})
            if tag == "HELLO":
                board = addr[0]
                print(f"  HELLO from {board}")
            elif tag == "BEGIN":
                in_phase = obj["idx"]
                next_ping = now + 1.5
                twt = obj.get("twt")
                agreed = (f", TWT {'agreed' if twt.get('ok') else 'REJECTED'} "
                          f"{twt.get('wake_invl_us', 0) / 1000:.0f} ms / {twt.get('wake_dura_us', 0) / 1000:.1f} ms"
                          if twt else "")
                print(f"  phase {in_phase}: {obj['band']}/{obj['mode']} ch {obj['channel']} rssi {obj['rssi']} "
                      f"{obj['phy']}{agreed}")
            elif tag == "END":
                in_phase = None
            elif tag == "DONE":
                t_done = now
                if not obj.get("ok"):
                    break
    finally:
        sock.close()
    return events, list(pings.values())


def analyse(events, pings, recorder):
    phases = []
    for beg in (e for e in events if e["tag"] == "BEGIN"):
        end = next((e for e in events if e["tag"] == "END" and e["idx"] == beg["idx"]), None)
        if end is None:
            continue
        ph = {k: beg[k] for k in ("idx", "band", "mode", "ok", "twt", "ip", "channel", "rssi", "phy")}
        ph.update({"pings_answered_by_board": end["pings"], "reports": end["reports"],
                   "connected_at_end": end["connected"], "duration_s": end["t"] - beg["t"]})
        mine = [p for p in pings if p["phase"] == beg["idx"]]
        rtts = sorted(p["rtt_ms"] for p in mine if p["rtt_ms"] is not None)
        ph["pings_sent"] = len(mine)
        ph["pings_lost"] = len(mine) - len(rtts)
        ph["rtt_ms"] = ({"median": statistics.median(rtts), "p90": rtts[int(0.9 * (len(rtts) - 1))],
                         "max": rtts[-1]} if rtts else None)
        if recorder:
            w = recorder.window(beg["t"] + EDGE_BEGIN_S, end["t"] - EDGE_END_S)
            ph["mean_ma"] = statistics.fmean(w) / 1000 if w else None
            mid = (beg["t"] + end["t"]) / 2
            trace = recorder.window(mid - TRACE_S / 2, mid + TRACE_S / 2)
            ph["trace_ua_1ms"] = [round(v, 1) for v in trace]
        phases.append(ph)
    sleep_ua = None
    done = next((e for e in events if e["tag"] == "DONE"), None)
    if recorder and done and done.get("ok"):
        w = recorder.window(done["t"] + SLEEP_WINDOW_S[0], done["t"] + SLEEP_WINDOW_S[1])
        sleep_ua = statistics.fmean(w) if w else None
    return phases, sleep_ua, done


def run_board(args):
    if args.configured:
        # Board already holds this plan with autorun set; it is not on USB.
        # 板子已存有该计划且开启自动运行，未接 USB。
        stored = args.out.parent / f".configured-{args.board}.json"
        if not stored.exists():
            sys.exit(f"{stored} not found; run once with the board on USB")
        info = json.loads(stored.read_text())
        plan = info["plan"]
        con = None
    else:
        env = load_env(ENV_FILE)
        con, info, plan = configure(args, env)
    n_phases = plan.count(",") + 1
    recorder = ppk = None
    if args.dry_run:
        # Board stays on USB and runs from the console with light sleep, as on battery; markers arrive over UDP.
        # 板子保持 USB 供电，从串口启动并与电池供电时一样浅睡；标记经 UDP 送达。
        con.send("start pm=1")
        # Background reader that keeps the board's console output flowing.
        # 后台读取线程，持续取走板子的串口输出。
        drain_stop = threading.Event()
        drain = threading.Thread(target=lambda: [con.ser.read(65536) for _ in iter(drain_stop.is_set, True)],
                                 daemon=True)
        drain.start()
        try:
            events, pings = collect(args, n_phases)
        finally:
            drain_stop.set()
            drain.join(1)
            con.close()
    else:
        ppk = open_ppk(args.ppk_port, args.mv)
        if con is not None:
            con.close()
            print(f"Unplug the USB cable from the XIAO now; the PPK2 then supplies {args.mv} mV", flush=True)
            while Path(args.port).exists():
                time.sleep(0.2)
            time.sleep(1.0)
        else:
            args.secs = info["secs"]
        recorder = Recorder(ppk)
        ppk.start_measuring()
        recorder.start()
        time.sleep(0.5)
        ppk.toggle_DUT_power("ON")
        print("  board powered by PPK2, waiting for it to join Wi-Fi")
        try:
            events, pings = collect(args, n_phases, recorder)
        finally:
            ppk.toggle_DUT_power("OFF")
            recorder.stop_evt.set()
            recorder.join(2)
            ppk.stop_measuring()
        print("  decoding the current trace", flush=True)
        recorder.decode()
    phases, sleep_ua, done = analyse(events, pings, recorder)
    if (done is None or not done.get("ok")) and recorder and recorder.bins:
        # Per-second mean and peak current, to tell a reboot loop from a hang.
        # 每秒的平均与峰值电流，用于区分反复重启与卡死。
        b = recorder.bins
        print("  current per second (mean/peak mA):")
        print("   " + " ".join(f"{statistics.fmean(b[i:i + 1000]) / 1000:.0f}/{max(b[i:i + 1000]) / 1000:.0f}"
                             for i in range(0, len(b), 1000)))
    if (done is None or not done.get("ok")) and recorder and args.port:
        # Read the board's reset log once it is back on USB.
        # 板子重新接上 USB 后读取其复位记录。
        print("  plug the USB cable back in to read the board's reset log", flush=True)
        t_wait = time.monotonic() + 180
        while not Path(args.port).exists() and time.monotonic() < t_wait:
            time.sleep(0.2)
        if Path(args.port).exists():
            time.sleep(2.0)
            con = Console(args.port)
            try:
                print(f"  resets: {con.command('info', 'RESETS', 5)['oldest_first']}")
            finally:
                con.close()
    if done is None or not done.get("ok"):
        sys.exit(f"[{args.board}] run did not finish: {done or 'no DONE marker'}; "
                 f"{len(phases)} phase(s) complete")
    meta = {"label": args.board, "note": args.note, "dry_run": args.dry_run,
            "recorded_at": datetime.now().isoformat(timespec="seconds"),
            "info": {k: v for k, v in info.items() if k not in ("ssid", "ssid5", "pass_set")},
            "config": {"plan": plan, "secs": args.secs, "supply_mv": None if args.dry_run else args.mv,
                       "report_every_s": 10, "ping_gap_s": PING_GAP_S},
            "phases": phases, "deep_sleep_ua": sleep_ua}
    name = f"{args.board}-dry" if args.dry_run else args.board
    (args.out / f"{name}.json").write_text(json.dumps(meta, indent=1))


# ---------------------------------------------------------------------------- report

def esc(text):
    return str(text).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")


def phase_label(ph):
    band = "2.4 GHz" if ph["band"] == "2g" else "5 GHz"
    if ph["mode"] == "ps":
        return f"{band} DTIM power save"
    twt = ph.get("twt") or {}
    wake = f", wake {twt['wake_dura_us'] / 1000:.0f} ms" if twt.get("wake_dura_us", 65280) < 60000 else ""
    return f"{band} TWT {twt.get('wake_invl_us', 0) / 1000:.0f} ms{wake}"


def measured(runs):
    return [r for r in runs if not r.get("dry_run")]


def draw_current(runs, path: Path):
    """Horizontal bars: mean current per board and phase."""
    width, left, right, top, bar_h, gap = 1000, 330, 110, 60, 20, 6
    rows = [(r, ph) for r in runs for ph in r["phases"] if ph.get("mean_ma") is not None]
    if not rows:
        return
    max_ma = max(ph["mean_ma"] for _, ph in rows)
    scale = (width - left - right) / max_ma
    height = top + (bar_h + gap) * len(rows) + 14 * len(runs) + 30
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
        f'font-family="Helvetica, Arial, sans-serif" font-size="13">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        '<text x="10" y="26" font-size="15" font-weight="bold">Mean current while connected and idle '
        '(same network task, lower is better)</text>',
    ]
    y = top
    for run in runs:
        for ph in run["phases"]:
            if ph.get("mean_ma") is None:
                continue
            color = "#98a2b3" if ph["mode"] == "ps" else "#1f6fb2"
            parts.append(f'<text x="10" y="{y + 14}"><tspan font-weight="bold">{esc(run["label"])}</tspan> '
                         f'{esc(phase_label(ph))}</text>')
            parts.append(f'<rect x="{left}" y="{y}" width="{ph["mean_ma"] * scale:.1f}" height="{bar_h}" '
                         f'fill="{color}"/>')
            parts.append(f'<text x="{left + ph["mean_ma"] * scale + 6:.1f}" y="{y + 15}">'
                         f'{ph["mean_ma"]:.2f} mA</text>')
            y += bar_h + gap
        y += 14
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def draw_traces(runs, path: Path):
    """One 3-second current trace (1 ms resolution) from the middle of every phase."""
    width, left, top, row_h, gap = 1000, 250, 50, 70, 16
    rows = [(r, ph) for r in runs for ph in r["phases"] if ph.get("trace_ua_1ms")]
    if not rows:
        return
    peak = max(max(ph["trace_ua_1ms"]) for _, ph in rows)
    height = top + (row_h + gap) * len(rows) + 20
    plot_w = width - left - 20
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}" '
        f'font-family="Helvetica, Arial, sans-serif" font-size="12">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="10" y="24" font-size="15" font-weight="bold">Current over {TRACE_S:g} s in each phase '
        f'(1 ms mean, full scale {peak / 1000:.0f} mA)</text>',
    ]
    y = top
    for run, ph in rows:
        tr = ph["trace_ua_1ms"]
        step = plot_w / max(1, len(tr) - 1)
        pts = " ".join(f"{left + i * step:.1f},{y + row_h - v / peak * row_h:.1f}" for i, v in enumerate(tr))
        color = "#98a2b3" if ph["mode"] == "ps" else "#1f6fb2"
        parts.append(f'<text x="10" y="{y + row_h / 2}"><tspan font-weight="bold">{esc(run["label"])}</tspan> '
                     f'{esc(phase_label(ph))}</text>')
        parts.append(f'<line x1="{left}" y1="{y + row_h}" x2="{left + plot_w}" y2="{y + row_h}" stroke="#d0d5dd"/>')
        parts.append(f'<polyline points="{pts}" fill="none" stroke="{color}" stroke-width="1"/>')
        y += row_h + gap
    parts.append("</svg>")
    path.write_text("\n".join(parts))


def write_summary(runs, path: Path):
    lines = [
        "# Wi-Fi power: DTIM power save and Wi-Fi 6 Target Wake Time",
        "",
        "- Every phase runs the same task: stay connected, answer a PING from the computer every 3-7 s "
        "(random) and send a 64-byte UDP report every 10 s. Only the power-save method changes.",
        "- *DTIM power save* is 802.11 modem sleep, waking for every DTIM beacon. *TWT* is an individual "
        "Target Wake Time agreement (Wi-Fi 6) negotiated with the router; the board then wakes only at the "
        "agreed interval.",
        "- The PPK2 powers the board at the battery pads with USB unplugged. Mean current covers each phase "
        f"minus {EDGE_BEGIN_S:g} s after the start marker and {EDGE_END_S:g} s before the end marker. "
        "Light sleep and CPU frequency scaling are on in every phase.",
        "- *Light-sleep floor* is the 5th percentile of the 1 ms current in the phase's 3-second trace: "
        "the current between Wi-Fi wake-ups while the connection is kept.",
        "- Ping time is the round trip seen by the computer; it shows how long a command waits for a "
        "sleeping board.",
        "",
        "| Board | Phase | TWT agreed (interval / wake) | Channel | RSSI | PHY | Mean current | vs DTIM PS | Light-sleep floor "
        "| Ping median | Ping p90 | Pings lost | Reports | Recorded | Note |",
        "|---|---|---|---:|---:|---|---:|---:|---:|---:|---:|---:|---:|---|---|",
    ]
    for run in runs:
        base = {ph["band"]: ph.get("mean_ma") for ph in run["phases"] if ph["mode"] == "ps"}
        for ph in run["phases"]:
            twt = ph.get("twt")
            agreed = ("-" if not twt else f"{twt['wake_invl_us'] / 1000:.0f} ms / {twt['wake_dura_us'] / 1000:.1f} ms"
                      if twt.get("ok") else f"rejected (status {twt.get('status')}, {twt.get('api_err')})")
            ma = ph.get("mean_ma")
            ref = base.get(ph["band"])
            ratio = f"{(ma / ref - 1) * 100:+.0f}%" if ma and ref and ph["mode"] != "ps" else "-"
            rtt = ph.get("rtt_ms") or {}
            tr = sorted(ph.get("trace_ua_1ms") or [])
            floor = f"{tr[len(tr) // 20] / 1000:.2f} mA" if tr else "-"
            lines.append(
                f"| {run['label']} | {phase_label(ph)} | {agreed} | {ph['channel']} | {ph['rssi']} dBm | {ph['phy']} "
                f"| {f'{ma:.2f} mA' if ma is not None else '-'} | {ratio} | {floor} "
                f"| {f'{rtt['median']:.0f} ms' if rtt else '-'} | {f'{rtt['p90']:.0f} ms' if rtt else '-'} "
                f"| {ph['pings_lost']}/{ph['pings_sent']} | {ph['reports']} | {run['recorded_at']} "
                f"| {run.get('note') or '-'} |")
    # TODO(deep sleep window reads tens of mA on C5; verify the board reaches deep sleep before reporting it):
    # the measured value stays in each <label>.json as deep_sleep_ua.
    lines += ["", "![mean current](current.svg)", "", "![current traces](traces.svg)", ""]
    path.write_text("\n".join(lines))


def report(out_dir: Path):
    runs = sorted((json.loads(p.read_text()) for p in out_dir.glob("*.json")), key=lambda r: r["recorded_at"])
    for run in runs:
        for ph in run["phases"]:
            ma = ph.get("mean_ma")
            rtt = ph.get("rtt_ms") or {}
            print(f"{run['label']:<8}{' (dry)' if run.get('dry_run') else '':<7} {phase_label(ph):<32} "
                  f"{f'{ma:6.2f} mA' if ma is not None else '     - mA'}  ping "
                  f"{f'{rtt['median']:.0f} ms' if rtt else '-'}  lost {ph['pings_lost']}/{ph['pings_sent']}")
    runs = measured(runs)
    if not runs:
        print("no PPK2 measurements yet; summary not written")
        return
    draw_current(runs, out_dir / "current.svg")
    draw_traces(runs, out_dir / "traces.svg")
    write_summary(runs, out_dir / "summary.md")
    print(f"report: {out_dir / 'summary.md'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="label for this run in the comparison, e.g. C5")
    ap.add_argument("--port", help="board serial port (default: the only /dev/cu.usbmodem* present)")
    ap.add_argument("--ppk-port", help="PPK2 serial port (default: first PPK2 found)")
    ap.add_argument("--host", help="this computer's IPv4 address on the test network (default: default route)")
    ap.add_argument("--plan", help="phases, e.g. 2g/ps,2g/twt500 (default depends on the chip)")
    ap.add_argument("--secs", type=int, default=60, help="seconds per phase, 10-600 (default 60)")
    ap.add_argument("--mv", type=int, default=3700, help="PPK2 supply voltage in mV (default 3700)")
    ap.add_argument("--dry-run", action="store_true", help="stay on USB without PPK2 to check Wi-Fi and markers")
    ap.add_argument("--note", default="", help="free-text test conditions saved with this result")
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="comparison folder that collects every run")
    ap.add_argument("--configured", action="store_true",
                    help="board is off USB and already holds the plan from an earlier run with this label")
    ap.add_argument("--replot", action="store_true", help="only rebuild summary.md and the charts")
    args = ap.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    if not args.replot:
        if not args.board:
            ap.error("give --board LABEL")
        if not 10 <= args.secs <= 600:
            ap.error("--secs must be 10-600")
        args.port = args.port or (None if args.configured else find_port())
        args.host = args.host or local_ip()
        run_board(args)
    report(args.out)


if __name__ == "__main__":
    main()
