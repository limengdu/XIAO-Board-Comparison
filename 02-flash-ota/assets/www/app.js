// Dashboard: reads /api/info, draws the flash layout and lists which resource packs are installed.

const KIB = 1024;
const fmtKiB = (n) => `${(n / KIB).toLocaleString(undefined, { maximumFractionDigits: 0 })} KiB`;

// One probe file per resource pack; a pack counts as installed when its probe is served.
const PACKS = [
  { name: "Web dashboard + Chart.js", probe: "vendor/chart.umd.js" },
  { name: "Font Awesome Free icons", probe: "fa/webfonts/fa-solid-900.woff2" },
  { name: "Inter UI font", probe: "fonts/inter/files/inter-latin-400-normal.woff2" },
  { name: "Noto Sans SC, regular", probe: "fonts/noto-sans-sc/400.css" },
  { name: "Noto Sans SC, bold", probe: "fonts/noto-sans-sc/700.css" },
];

async function exists(path) {
  try {
    const resp = await fetch(path, { method: "GET", cache: "no-store" });
    return resp.ok;
  } catch {
    return false;
  }
}

function drawLayout(info) {
  const appParts = info.partitions.filter((p) => p.type === 0);
  const fsPart = info.partitions.find((p) => p.label === "storage");
  const first = Math.min(...info.partitions.map((p) => p.offset));
  const fsUsed = info.fs_total - info.fs_free;
  const rows = [
    ["Bootloader, table, NVS", first + info.partitions.filter((p) => p.type === 1 && p.label !== "storage")
      .reduce((s, p) => s + p.size, 0), "#98a2b3"],
    ...appParts.map((p) => [
      p.label === info.running ? `${p.label} (running, ${fmtKiB(info.app_bytes)} used)` : `${p.label} (update slot)`,
      p.size, p.label === info.running ? "#1f6fb2" : "#84caff"]),
    ["Files used", fsUsed, "#f79009"],
    ["Files free", info.fs_free, "#fedf89"],
    ["FAT overhead", fsPart ? fsPart.size - info.fs_total : 0, "#d0d5dd"],
  ];
  if (window.Chart) {
    new Chart(document.getElementById("flash"), {
      type: "bar",
      data: {
        labels: ["Flash"],
        datasets: rows.map(([label, size, color]) => ({ label, data: [size / KIB], backgroundColor: color })),
      },
      options: {
        indexAxis: "y",
        plugins: { legend: { position: "bottom" }, tooltip: { callbacks: { label: (c) => `${c.dataset.label}: ${fmtKiB(c.raw * KIB)}` } } },
        scales: { x: { stacked: true, max: info.flash_bytes / KIB, title: { display: true, text: "KiB" } }, y: { stacked: true } },
      },
    });
  }
  document.getElementById("parts").innerHTML =
    "<tr><th>Region</th><th class='num'>Size</th></tr>" +
    rows.map(([label, size]) => `<tr><td>${label}</td><td class="num">${fmtKiB(size)}</td></tr>`).join("");
}

async function main() {
  const info = await (await fetch("/api/info", { cache: "no-store" })).json();
  document.getElementById("board").textContent = `XIAO ${info.target.replace("esp32", "ESP32-").toUpperCase()}`;
  document.getElementById("version").textContent =
    `Firmware ${info.version} on ${info.running} · ${info.flash_bytes / KIB / KIB} MB flash · ESP-IDF ${info.idf}`;
  drawLayout(info);

  const list = document.getElementById("resources");
  for (const pack of PACKS) {
    const ok = await exists(pack.probe);
    const li = document.createElement("li");
    li.innerHTML = `<i class="fa-solid ${ok ? "fa-circle-check ok" : "fa-circle-xmark missing"}"></i> ` +
      `${pack.name} <span class="${ok ? "ok" : "missing"}">${ok ? "installed" : "not installed"}</span>`;
    list.appendChild(li);
  }

  const sample = await fetch("sample-zh.txt").then((r) => (r.ok ? r.text() : "")).catch(() => "");
  document.getElementById("sample").textContent = sample;
  document.getElementById("sample-bold").textContent = sample;
}

main();
