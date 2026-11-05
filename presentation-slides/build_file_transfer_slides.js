// Builds presentation-slides/File-Transfer-Server.pptx from report/file-transfer/data.json.
//
// Usage (from the repo root):
//   python report/file-transfer/make_figures.py      # refresh data.json
//   npm install pptxgenjs                              # once
//   node presentation-slides/build_file_transfer_slides.js
//
// Optional: set APPLY_THEME=/path/to/apply_theme.js to write the theme colours into the
// deck (otherwise PowerPoint's stock theme colours are used for scheme colours).
const path = require("path");
const pptxgen = require("pptxgenjs");

const REPO = path.resolve(__dirname, "..");
const OUT = path.join(REPO, "presentation-slides", "File-Transfer-Server.pptx");
const DATAFILE = require(path.join(REPO, "report", "file-transfer", "data.json"));
const DATA = DATAFILE.cells;

const THEME = {
  name: "NetworkIO",
  headFontFace: "Cambria",
  bodyFontFace: "Calibri",
  colors: {
    dk1: "1B1F24", lt1: "FFFFFF", dk2: "14283C", lt2: "EEF2F6",
    accent1: "2A78D6", accent2: "EB6834", accent3: "1BAF7A", accent4: "EDA100",
    accent5: "6250D6", accent6: "E34948", hlink: "2A78D6", folHlink: "6250D6",
  },
};
const HEX = THEME.colors;
const SERIES = ["select", "poll", "epoll", "uring"];
const SERIES_LABEL = ["select", "poll", "epoll", "io_uring"];
const SERIES_COLORS = [HEX.accent1, HEX.accent2, HEX.accent3, HEX.accent4];

const pres = new pptxgen();
pres.layout = "LAYOUT_WIDE"; // 13.33 x 7.5
pres.theme = { headFontFace: THEME.headFontFace, bodyFontFace: THEME.bodyFontFace };
pres.title = "File Transfer Server: I/O Backend Benchmarks";
pres.author = "CS331 Team T008";
const C = pres.SchemeColor;

// ---------- layouts ----------
pres.defineSlideMaster({
  title: "DARK_TITLE",
  background: { color: HEX.dk2 },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: 0.8, y: 2.3, w: 11.7, h: 1.9, fontSize: 44, bold: true, color: C.background1, valign: "bottom", align: "left", fontFace: THEME.headFontFace }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: 0.8, y: 4.4, w: 11.7, h: 1.4, fontSize: 20, color: "CADCFC", valign: "top" }, text: "" } },
  ],
});
pres.defineSlideMaster({
  title: "CONTENT",
  background: { color: HEX.lt1 },
  objects: [
    { placeholder: { options: { name: "title", type: "title", x: 0.6, y: 0.85, w: 12.1, h: 0.75, fontSize: 36, bold: true, color: C.text2, valign: "middle", align: "left", margin: 0, fontFace: THEME.headFontFace }, text: "" } },
    { placeholder: { options: { name: "body", type: "body", x: 0.6, y: 1.62, w: 12.1, h: 0.5, fontSize: 18, color: C.accent1, valign: "top", margin: 0 }, text: "" } },
    { text: { text: "CS331 T008 · File Transfer Server", options: { x: 0.6, y: 7.0, w: 6, h: 0.3, fontSize: 10, color: "6B7280", margin: 0 } } },
  ],
  slideNumber: { x: 12.2, y: 7.0, w: 0.5, h: 0.3, fontSize: 10, color: "6B7280", align: "right" },
});

// ---------- helpers ----------
const med = (key, b, m) => { const c = DATA[key][b][m]; return c ? c.median : null; };
const r2 = (v) => Math.round(v * 100) / 100;
const K = (w, c, f) => `${w}|c${c}|${f}`;

function tag(slide, label) {
  slide.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 0.6, y: 0.4, w: 1.1, h: 0.36, rectRadius: 0.18, fill: { color: C.text2 }, line: { type: "none" }, objectName: "Experiment tag",
  });
  slide.addText(label, {
    x: 0.6, y: 0.4, w: 1.1, h: 0.36, align: "center", valign: "middle", fontSize: 13, bold: true,
    color: C.background1, margin: 0, isTextBox: true, objectName: "Experiment tag label",
  });
}

function content(section, tagLabel, title, takeaway) {
  const s = pres.addSlide({ masterName: "CONTENT", sectionTitle: section });
  tag(s, tagLabel);
  s.addText(title, { placeholder: "title" });
  s.addText(takeaway, { placeholder: "body" });
  return s;
}

const AXIS = {
  catAxisLabelColor: "52514E", valAxisLabelColor: "52514E",
  catAxisLabelFontFace: "+mn-lt", valAxisLabelFontFace: "+mn-lt",
  catAxisLabelFontSize: 12, valAxisLabelFontSize: 11,
  valAxisTitleFontFace: "+mn-lt", valAxisTitleFontSize: 12, valAxisTitleColor: "52514E",
  catAxisTitleFontFace: "+mn-lt", catAxisTitleFontSize: 12, catAxisTitleColor: "52514E",
  titleFontFace: "+mn-lt", titleFontSize: 14, titleColor: HEX.dk2,
  legendFontFace: "+mn-lt", legendFontSize: 12, legendColor: "1B1F24",
  valGridLine: { color: "E4E3DF", size: 0.75 }, catGridLine: { style: "none" },
  showLegend: true, legendPos: "b", chartColors: SERIES_COLORS,
};

function barData(cells, metric, labels, scale = 1) {
  return SERIES.map((b, i) => ({
    name: SERIES_LABEL[i], labels,
    values: cells.map((k) => r2(med(k, b, metric) * scale)),
  }));
}

function barChart(slide, data, opts) {
  slide.addChart(pres.charts.BAR, data, {
    barDir: "col", barGrouping: "clustered", barGapWidthPct: 60, barOverlapPct: -8,
    ...AXIS, showTitle: !!opts.title, title: opts.title,
    showValAxisTitle: true, valAxisTitle: opts.yTitle, valAxisMinVal: 0,
    valAxisLabelFormatCode: opts.fmt || "#,##0",
    showCatAxisTitle: !!opts.xTitle, catAxisTitle: opts.xTitle,
    x: opts.x, y: opts.y, w: opts.w, h: opts.h, objectName: opts.name,
  });
}

function bullets(slide, items, box) {
  slide.addText(
    items.map((t, i) => ({ text: t, options: { bullet: true, breakLine: i < items.length - 1, paraSpaceAfter: 8 } })),
    { ...box, fontSize: 15, color: C.text1, valign: "top", isTextBox: true, objectName: "Commentary" },
  );
}

function sidePanel(slide, heading, items) {
  slide.addShape(pres.shapes.ROUNDED_RECTANGLE, {
    x: 8.75, y: 2.35, w: 3.95, h: 4.4, rectRadius: 0.12, fill: { color: C.background2 }, line: { type: "none" }, objectName: "Commentary panel",
  });
  slide.addText(heading, { x: 9.0, y: 2.5, w: 3.5, h: 0.45, fontSize: 16, bold: true, color: C.text2, margin: 0, isTextBox: true, objectName: "Panel heading" });
  bullets(slide, items, { x: 9.0, y: 3.0, w: 3.5, h: 3.6, margin: 0 });
}

const BOTTOM = { x: 0.6, y: 5.75, w: 12.1, h: 1.15, margin: 0 };

// ================= 1. Title =================
pres.addSection({ title: "Introduction" });
let s = pres.addSlide({ masterName: "DARK_TITLE", sectionTitle: "Introduction" });
s.addText("File Transfer Server: select vs poll vs epoll vs io_uring", { placeholder: "title" });
s.addText("Benchmark results for HTTP file serving across four Linux I/O backends\nCS331 Computer Networks · Team T008", { placeholder: "body" });
s.addNotes("The same file server code runs on all four backends; only the event-notification layer changes. This deck covers experiments E5a, E5b, E7, E8 and E9, then explains why the backends look alike and where they actually differ.");

// ================= 2. Key findings =================
s = content("Introduction", "Summary", "Key findings", "Backends tie on throughput because the server is never saturated; they differ in CPU cost");
const stats = [
  ["≤ 48%", "Server CPU core busy in every test: never saturated"],
  ["−12%", "io_uring server CPU per response on large files (vs epoll)"],
  ["+27%", "poll server CPU per response at 1000 connections"],
  ["−62%", "4 KiB throughput with TCP_NODELAY off (Nagle + delayed ACK)"],
  ["−41%", "Throughput with 16 KiB chunks instead of 64 KiB"],
  ["13.5 s", "io_uring p99 latency at 1000 connections (epoll: 0.24 s)"],
];
stats.forEach(([num, label], i) => {
  const col = i % 3, row = Math.floor(i / 3);
  const x = 0.6 + col * 4.1, y = 2.35 + row * 2.3;
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y, w: 3.85, h: 2.0, rectRadius: 0.12, fill: { color: C.background2 }, line: { type: "none" }, objectName: `Stat card ${i + 1}` });
  s.addText(num, { x: x + 0.3, y: y + 0.2, w: 3.3, h: 0.95, fontSize: 44, bold: true, color: C.text2, fontFace: THEME.headFontFace, margin: 0, valign: "middle", isTextBox: true, objectName: `Stat value ${i + 1}` });
  s.addText(label, { x: x + 0.3, y: y + 1.15, w: 3.3, h: 0.7, fontSize: 14, color: "52514E", margin: 0, valign: "top", isTextBox: true, objectName: `Stat label ${i + 1}` });
});
s.addNotes("Headline: throughput and mean latency are nearly identical across backends, but that is because the server's core is never more than about 48% busy, so the client and loopback path set the limit. Where the backends do differ is CPU cost per response, and at 1000 connections. TCP_NODELAY and chunk size matter far more than the backend.");

// ================= 3. Setup =================
s = content("Introduction", "Setup", "Test setup", "Loopback benchmark, server pinned to one core, 5 repeats per cell, median reported");
const flow = [["wrk client", "4 threads · CPUs 2–5"], ["Loopback TCP", "127.0.0.1, HTTP/1.1"], ["filesrv", "1 thread · CPU 1"], ["File on disk", "read() → write(), 64 KiB chunks"]];
flow.forEach(([h, sub], i) => {
  const x = 0.6 + i * 3.1;
  s.addShape(pres.shapes.ROUNDED_RECTANGLE, { x, y: 2.4, w: 2.6, h: 1.25, rectRadius: 0.12, fill: { color: i === 2 ? C.text2 : C.background2 }, line: { type: "none" }, objectName: `Flow box ${i + 1}` });
  s.addText([{ text: h, options: { bold: true, fontSize: 17, breakLine: true } }, { text: sub, options: { fontSize: 13 } }], {
    x, y: 2.4, w: 2.6, h: 1.25, align: "center", valign: "middle", color: i === 2 ? C.background1 : C.text2, margin: 0.05, isTextBox: true, objectName: `Flow text ${i + 1}`,
  });
  if (i < flow.length - 1) s.addShape(pres.shapes.RIGHT_ARROW, { x: x + 2.68, y: 2.87, w: 0.36, h: 0.32, fill: { color: "9AA3AE" }, line: { type: "none" }, objectName: `Flow arrow ${i + 1}` });
});
const cards = [
  ["Machine", ["Intel i5-10300H, 4 cores / 8 threads", "WSL2, Linux 6.18.54", "wrk gets 2 physical cores"]],
  ["Server", ["Same filesrv.c on every backend", "No sendfile or mmap", "Header and body sent as two writes", "TCP_NODELAY on by default"]],
  ["Method", ["10 s warm-up, 30 s measured (E8: 90 s)", "5 repeats per cell, median shown", "357 of 360 runs accepted"]],
];
cards.forEach(([h, items], i) => {
  const x = 0.6 + i * 4.1;
  s.addText(h, { x, y: 4.05, w: 3.85, h: 0.4, fontSize: 18, bold: true, color: C.text2, margin: 0, isTextBox: true, objectName: `Setup heading ${i + 1}` });
  bullets(s, items, { x, y: 4.5, w: 3.85, h: 2.3, margin: 0 });
});
s.addNotes("Everything runs over loopback on one laptop, so there is no real network RTT. The server is single-threaded and pinned to CPU 1. wrk is pinned to CPUs 2 to 5, which are hyperthread pairs on only two physical cores; this matters later.");

// ================= E5a =================
pres.addSection({ title: "E5a File size vs concurrency" });
const E5A = "e5a-size-concurrency";
const concKeys = [1, 100, 1000].map((c) => K(E5A, c, "4KiB"));
const concLabels = ["1", "100", "1000"];

s = content("E5a File size vs concurrency", "E5a", "Throughput vs concurrency", "All backends peak at 100 connections; poll falls furthest at 1000");
s.addChart(pres.charts.LINE, barData(concKeys, "rps", concLabels), {
  ...AXIS, lineSize: 2.5, lineDataSymbol: "circle", lineDataSymbolSize: 8,
  showValAxisTitle: true, valAxisTitle: "Responses/s (4 KiB file)", valAxisMinVal: 0, valAxisLabelFormatCode: "#,##0",
  showCatAxisTitle: true, catAxisTitle: "Concurrent connections",
  x: 0.6, y: 2.35, w: 7.9, h: 4.45, objectName: "E5a throughput vs concurrency chart",
});
sidePanel(s, "What we see", [
  "1 → 100 conns: +37% (4.3k → 5.9k req/s) on every backend; the server always has work waiting.",
  `1000 conns: poll drops to ${Math.round(med(concKeys[2], "poll", "rps")).toLocaleString()} req/s vs ${Math.round(med(concKeys[2], "epoll", "rps")).toLocaleString()} for epoll.`,
  "poll rebuilds and rescans its 1000-entry pollfd array in user space every loop (3 passes); epoll returns only ready sockets.",
]);
s.addNotes("poll's loop makes three passes over the whole pollfd array with a connection lookup each time; select makes two. poll's user CPU rises from about 2% to 9% at 1000 connections, and its CPU per response is 27% higher than epoll's.");

s = content("E5a File size vs concurrency", "E5a", "Average latency vs concurrency", "Latency grows with queue length; io_uring shows a starvation tail at 1000");
s.addChart(pres.charts.LINE, barData(concKeys, "lat_avg_ms", concLabels), {
  ...AXIS, lineSize: 2.5, lineDataSymbol: "circle", lineDataSymbolSize: 8,
  showValAxisTitle: true, valAxisTitle: "Mean latency (ms, log scale)", valAxisLogScaleBase: 10, valAxisMinVal: 0.1, valAxisCrossesAt: "min", valAxisLabelFormatCode: "#,##0.0",
  showCatAxisTitle: true, catAxisTitle: "Concurrent connections",
  x: 0.6, y: 2.35, w: 7.9, h: 4.45, objectName: "E5a latency vs concurrency chart",
});
sidePanel(s, "What we see", [
  "Little's law holds: latency = connections ÷ throughput (100 / 5,925 ≈ 17 ms; 1000 / 5,484 ≈ 182 ms).",
  `io_uring at 1000: mean ${Math.floor(med(concKeys[2], "uring", "lat_avg_ms"))} ms, p99 13.5 s, with the same throughput as epoll.`,
  "Seen in all 5 runs: a fairness problem, not a speed problem. Root cause not yet confirmed.",
]);
s.addNotes("wrk sends the next request as soon as the previous one completes, so mean latency mostly mirrors throughput. io_uring's tail is likely about the order in which completions are processed and receives re-submitted with 1000 receives in flight.");

s = content("E5a File size vs concurrency", "E5a", "Throughput vs file size", "Small files are request-bound; large files hit the same ~170 MB/s ceiling");
const sizeKeys = [K(E5A, 100, "4KiB"), K(E5A, 10, "64KiB"), K(E5A, 100, "1MiB")];
barChart(s, barData(sizeKeys, "mbps", ["4 KiB (100 conns)", "64 KiB (10 conns)", "1 MiB (100 conns)"]), {
  yTitle: "Throughput (MB/s)", x: 0.6, y: 2.35, w: 7.9, h: 4.45, name: "E5a throughput vs file size chart",
});
sidePanel(s, "What we see", [
  "4 KiB: ~24.5 MB/s. The fixed cost of each request dominates.",
  "64 KiB and 1 MiB: ~170 MB/s on all four backends.",
  "The server core is only ~24% busy at that ceiling, so the limit is outside the server.",
]);
s.addNotes("Caution: this chart mixes concurrency levels. 64 KiB was only tested at 10 connections, and the other sizes at 100.");

// ================= E5b =================
pres.addSection({ title: "E5b Connection behaviour" });
s = content("E5b Connection behaviour", "E5b", "Close vs keep-alive", "Keep-alive measured +5 to +8%, but that is within session-to-session variation");
const e5b = [K("e5b-close-per-response", 100, "4KiB"), K("e5b-keepalive-c300", 100, "4KiB")];
const e5bLabels = ["Connection: close", "Keep-alive"];
barChart(s, barData(e5b, "rps", e5bLabels), { title: "Throughput", yTitle: "Responses/s", x: 0.6, y: 2.3, w: 6.0, h: 3.35, name: "E5b throughput chart" });
barChart(s, barData(e5b, "lat_avg_ms", e5bLabels), { title: "Average latency", yTitle: "Mean latency (ms)", x: 6.75, y: 2.3, w: 6.0, h: 3.35, name: "E5b latency chart", fmt: "0" });
bullets(s, [
  "Keep-alive skips accept(), close() and per-socket backend setup. io_uring pays the most per new connection (+15% CPU per response).",
  "But this keep-alive test ran 7–12% below the identical E5a test, a bigger gap than the effect, so the throughput result is inconclusive.",
], BOTTOM);
s.addNotes("4 KiB file, 100 connections, max-conns 300 on all backends. The two halves ran in separate sessions. On loopback a TCP handshake is almost free; on a real network each new connection costs at least one extra round trip.");

// ================= E7 =================
pres.addSection({ title: "E7 Buffer sensitivity" });
s = content("E7 Buffer sensitivity", "E7", "Buffer and chunk size", "Chunk size matters (−41% at 16 KiB); socket buffer sizes do not");
const e7 = ["e7-baseline", "e7-chunk16", "e7-rcvbuf512", "e7-sndbuf64"].map((w) => K(w, 100, "4MiB"));
const e7Labels = ["Baseline", "16 KiB chunk", "RCVBUF 512 KiB", "SNDBUF 64 KiB"];
const e7X = "Setting (baseline = 64 KiB chunk, default socket buffers)";
barChart(s, barData(e7, "mbps", e7Labels), { title: "Throughput", yTitle: "Throughput (MB/s)", x: 0.6, y: 2.3, w: 6.0, h: 3.35, name: "E7 throughput chart", xTitle: e7X });
barChart(s, barData(e7, "lat_avg_ms", e7Labels, 0.001), { title: "Average latency per 4 MiB file", yTitle: "Mean latency (s)", x: 6.75, y: 2.3, w: 6.0, h: 3.35, name: "E7 latency chart", fmt: "0.0", xTitle: e7X });
bullets(s, [
  "16 KiB chunks mean 4× the read → write cycles and 2.4× the server CPU per file. The server is still only ~34% busy, so the loss likely comes from smaller writes on the client and loopback side.",
  "SO_RCVBUF does nothing (the server only receives a small GET); a 64 KiB SO_SNDBUF still holds a whole chunk.",
], BOTTOM);
s.addNotes("4 MiB file, 100 connections, application buffer 256 KiB in every run, so --buf-size itself was never varied. The client was not measured, so the chunk-size explanation is likely but not confirmed.");

// ================= E8 =================
pres.addSection({ title: "E8 Slow readers" });
s = content("E8 Slow readers", "E8", "Slow readers", "Non-blocking servers absorb 10 slow readers; 100 cost 7–11%");
const e8 = [0, 10, 100].map((n) => K(`e8-slow${n}`, 100, "100MiB"));
const e8Labels = ["0 slow readers", "10 slow readers", "100 slow readers"];
barChart(s, barData(e8, "mbps", e8Labels), { title: "Fast-client throughput", yTitle: "Throughput (MB/s)", x: 0.6, y: 2.3, w: 6.0, h: 3.35, name: "E8 throughput chart" });
barChart(s, barData(e8, "lat_avg_ms", e8Labels, 0.001), { title: "Time to download 100 MiB", yTitle: "Mean latency (s)", x: 6.75, y: 2.3, w: 6.0, h: 3.35, name: "E8 latency chart", fmt: "0" });
bullets(s, [
  "A slow reader's write just waits (EAGAIN or a pending SQE) while the loop serves others: it costs memory, not loop time.",
  "Optimistic: run 3 at 100 readers (no download finished in 90 s) was rejected for select, poll and epoll, so n = 4.",
], BOTTOM);
s.addNotes("100 fast wrk clients plus 0, 10 or 100 curl readers throttled to 100 KiB/s, 90 second window. 100 readers × 100 KiB/s is about 10 MB/s, close to what the fast clients lose. The rejected run was the worst case and hit the same run on three backends, which points to a host slowdown. One download takes about 62 of the 90 s, so the window is short.");

// ================= E9 =================
pres.addSection({ title: "E9 TCP_NODELAY" });
s = content("E9 TCP_NODELAY", "E9", "TCP_NODELAY", "Turning Nagle on cuts 4 KiB throughput by 62% on every backend");
const e9 = [K("e9-nodelay1", 100, "4KiB"), K("e9-nodelay0", 100, "4KiB"), K("e9-nodelay1", 100, "64KiB"), K("e9-nodelay0", 100, "64KiB")];
const e9Labels = ["4 KiB · on", "4 KiB · off", "64 KiB · on", "64 KiB · off"];
barChart(s, barData(e9, "rps", e9Labels), { title: "Throughput", yTitle: "Responses/s", x: 0.6, y: 2.3, w: 6.0, h: 3.35, name: "E9 throughput chart", xTitle: "File size · TCP_NODELAY" });
barChart(s, barData(e9, "lat_avg_ms", e9Labels), { title: "Average latency", yTitle: "Mean latency (ms)", x: 6.75, y: 2.3, w: 6.0, h: 3.35, name: "E9 latency chart", fmt: "0", xTitle: "File size · TCP_NODELAY" });
bullets(s, [
  "Header and body go out as two writes; Nagle holds the small body until the header is ACKed, which the client delays ~40 ms. 100 conns ÷ 44 ms ≈ 2,270 req/s, exactly as measured.",
  "64 KiB: only a 53-byte tail is held, and the full segment before it makes the client ACK at once. Fix: keep NODELAY on, or use one writev().",
], BOTTOM);
s.addNotes("Only TCP_NODELAY changes. On loopback a segment is about 64 KiB, so a 4 KiB body is a small segment that Nagle holds. The server core is only about 4% busy with Nagle on, because it is just waiting. The effect is identical on every backend because it happens in the kernel TCP stack.");

// ================= Why they look the same =================
pres.addSection({ title: "Why the backends look the same" });
s = content("Why the backends look the same", "Analysis", "Why do all four look the same?", "The server core is never saturated, so the client and loopback set the limit");
const core = DATAFILE.core_utilisation.slice().reverse(); // bar charts draw bottom-up
const coreLabels = core.map((r) => `${r.label} (${Math.round(r.busy)}%)`);
s.addChart(pres.charts.BAR, [
  { name: "User space", labels: coreLabels, values: core.map((r) => r2(r.user)) },
  { name: "Kernel (syscalls)", labels: coreLabels, values: core.map((r) => r2(r.sys)) },
  { name: "Kernel (network softirq)", labels: coreLabels, values: core.map((r) => r2(r.softirq)) },
], {
  barDir: "bar", barGrouping: "stacked", barGapWidthPct: 45, ...AXIS,
  chartColors: [HEX.accent1, HEX.accent2, HEX.accent3], catAxisLabelFontSize: 11,
  valAxisMinVal: 0, valAxisMaxVal: 100, valAxisMajorUnit: 20, valAxisLabelFormatCode: "0",
  showValAxisTitle: true, valAxisTitle: "Server CPU core busy (%)",
  x: 0.6, y: 2.25, w: 7.9, h: 4.6, objectName: "Server core utilisation chart",
});
sidePanel(s, "What we see", [
  "Every test leaves the server core more than half idle (4–48% busy).",
  "The backend-specific wait step runs ~180 times/s and returns ~33 responses per wakeup, against ~17,700 read/write calls/s: about 1% of the work.",
  "Likely limit: wrk on 2 physical cores (client CPU not recorded).",
]);
s.addNotes("Busy share of the server's CPU core from /proc/stat during the measured window, including network softirq work, median of all accepted runs across the four backends. Going from 1 to 100 connections raises throughput 37% while the core gets slightly less busy, which a server-bound test would not do. Reading, file I/O and writing are identical code on every backend; only the wait step differs.");

s = content("Why the backends look the same", "Analysis", "Where the backends differ: CPU cost", "io_uring is ~12% cheaper per large file; poll is 27% dearer at 1000 connections");
const rel = DATAFILE.cpu_per_response_vs_epoll;
const relLabels = rel.map((r) => r.label);
s.addChart(pres.charts.BAR, ["select", "poll", "uring"].map((b) => ({
  name: b === "uring" ? "io_uring" : b, labels: relLabels, values: rel.map((r) => r2(r[b])),
})), {
  barDir: "col", barGrouping: "clustered", barGapWidthPct: 60, ...AXIS,
  chartColors: [HEX.accent1, HEX.accent2, HEX.accent4], catAxisLabelFontSize: 11,
  valAxisMinVal: -20, valAxisMaxVal: 30, valAxisMajorUnit: 10, valAxisLabelFormatCode: "+0;-0;0",
  catAxisLabelPos: "low",
  showValAxisTitle: true, valAxisTitle: "Server CPU per response vs epoll (%)",
  x: 0.6, y: 2.25, w: 7.9, h: 4.6, objectName: "CPU per response vs epoll chart",
});
sidePanel(s, "What we see", [
  "io_uring: −12 to −13% on 1 MiB to 100 MiB files, mostly because our other backends copy each chunk one extra time.",
  "poll: +27% at 1000 connections, from O(n) user-space scans.",
  "io_uring: +15% when every response opens a new connection.",
  "On a saturated server, these would become throughput gaps.",
]);
s.addNotes("Server CPU per response is the server process's user plus kernel CPU time divided by responses served, as a percentage relative to epoll (0 means equal to epoll). Lower is cheaper. These differences do not appear in throughput because the server is never the bottleneck. Caveat on io_uring: select, poll and epoll pread each file chunk into a staging slot and then memcpy it into the send buffer, while io_uring preads straight into the send buffer. That extra copy accounts for most of io_uring's saving, so it is partly an implementation artefact.");

// ================= Conclusions =================
pres.addSection({ title: "Conclusions" });
s = pres.addSlide({ masterName: "DARK_TITLE", sectionTitle: "Conclusions" });
s.addText("Conclusions", { x: 0.8, y: 0.6, w: 11.7, h: 0.9, fontSize: 40, bold: true, color: C.background1, fontFace: THEME.headFontFace, margin: 0, isTextBox: true, objectName: "Conclusions title" });
const concl = [
  ["Tie on throughput: server never saturated", "The core is ≤ 48% busy and the backend-specific step is ~1% of the work, so the client and loopback set the limit."],
  ["Real differences are in CPU cost and the tail", "io_uring ~12% cheaper per large file; poll +27% at 1000 connections; io_uring p99 of 13.5 s at 1000."],
  ["Tune TCP and chunking first", "TCP_NODELAY (2.6× latency) and chunk size (−41%) matter far more than the backend."],
];
concl.forEach(([h, body], i) => {
  const y = 1.85 + i * 1.55;
  s.addShape(pres.shapes.OVAL, { x: 0.8, y: y + 0.05, w: 0.7, h: 0.7, fill: { color: C.accent4 }, line: { type: "none" }, objectName: `Conclusion number ${i + 1}` });
  s.addText(String(i + 1), { x: 0.8, y: y + 0.05, w: 0.7, h: 0.7, align: "center", valign: "middle", fontSize: 22, bold: true, color: C.text2, margin: 0, isTextBox: true, objectName: `Conclusion digit ${i + 1}` });
  s.addText([{ text: h, options: { bold: true, fontSize: 22, color: "FFFFFF", breakLine: true } }, { text: body, options: { fontSize: 16, color: "CADCFC" } }], {
    x: 1.8, y, w: 10.7, h: 1.25, valign: "top", margin: 0, isTextBox: true, objectName: `Conclusion text ${i + 1}`,
  });
});
s.addNotes("Full numbers and methodology are in report/file-transfer/README.md, and every chart can be regenerated with make_figures.py.");

// ================= Limitations =================
s = content("Conclusions", "Next", "Limitations and next steps", "What we would change to make the backend comparison conclusive");
const hdr = { bold: true, color: "FFFFFF", fill: { color: HEX.dk2 }, fontSize: 15 };
const rows = [
  [{ text: "Limitation", options: hdr }, { text: "Fix", options: hdr }],
  ["Server never saturated; client likely the bottleneck", "Cap the server with a cgroup (cpu.max = 25% of a core); keep only runs where it was throttled"],
  ["Client CPU not recorded; wrk on 2 physical cores", "Record client_cpu_stat; give wrk its own cores or a second machine"],
  ["Closed-loop latency just mirrors throughput", "Add a fixed-rate test (wrk2 -R) to measure latency at a set load"],
  ["E5b halves ran in separate sessions", "Rerun close and keep-alive together, in random order"],
  ["Loopback on WSL2; 5 runs, no significance tests", "Repeat key tests over a real network; more runs plus confidence intervals"],
];
s.addTable(rows, {
  x: 0.6, y: 2.35, w: 12.1, colW: [5.2, 6.9], fontSize: 14, color: HEX.dk1, valign: "middle",
  border: { type: "solid", pt: 0.75, color: "D5D9E0" }, fill: { color: "FFFFFF" }, rowH: 0.62, margin: [0.06, 0.12, 0.06, 0.12],
  objectName: "Limitations table",
});
s.addNotes("If we had more time, the single most useful change is capping the server's CPU so it becomes the bottleneck; then the CPU-per-response differences would show up as throughput differences.");

(async () => {
  await pres.writeFile({ fileName: OUT });
  if (process.env.APPLY_THEME) {
    const { applyTheme } = require(process.env.APPLY_THEME);
    await applyTheme(OUT, THEME);
  }
  console.log("wrote", OUT);
})();
