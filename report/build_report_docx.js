// Builds report/CS331-T008-Report.docx (course report in the CS331 template format).
//
// Usage (from the repo root):
//   python report/file-transfer/make_figures.py
//   python report/file-transfer/make_report_figures.py
//   npm install docx            # once
//   node report/build_report_docx.js
//
// Text highlighted in yellow and starting with "[ECHO SERVER" is a placeholder for the
// echo-server owners to replace. Delete the instruction text once filled.
const fs = require("fs");
const path = require("path");
const {
  Document, Packer, Paragraph, TextRun, ImageRun, Table, TableRow, TableCell, AlignmentType,
  WidthType, ShadingType, BorderStyle, LevelFormat, PageNumber, Footer, HeadingLevel, ExternalHyperlink,
} = require("docx");

const REPO = path.resolve(__dirname, "..");
const FIG = path.join(REPO, "report", "file-transfer", "figures", "report");
const OUT = path.join(REPO, "report", "CS331-T008-Report.docx");
const GH = "https://github.com/Pallav-Biyala/CS331-T008-NetworkIO";

const FONT = "Times New Roman";
const SZ = 20;            // 10 pt
const TEXT_W = 9026;      // A4 width 11906 - 2 x 1440 margins (DXA)

// ---------- helpers ----------
function runs(parts, base = {}) {
  // parts: string or array of strings / {t, b, i, code, hl}
  const arr = Array.isArray(parts) ? parts : [parts];
  return arr.map((p) => {
    if (typeof p === "string") return new TextRun({ text: p, font: FONT, size: SZ, ...base });
    if (p instanceof ExternalHyperlink) return p;
    const o = { text: p.t, font: p.code ? "Courier New" : FONT, size: p.code ? 18 : SZ, bold: p.b, italics: p.i, ...base };
    if (p.hl) o.highlight = "yellow";
    return new TextRun(o);
  });
}
const P = (parts, opt = {}) => new Paragraph({
  children: runs(parts), alignment: opt.align || AlignmentType.JUSTIFIED,
  spacing: { after: opt.after ?? 80, before: opt.before ?? 0, line: 240 }, keepNext: opt.keepNext,
});
const H1 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_1, children: [new TextRun({ text: t, font: FONT })] });
const H2 = (t) => new Paragraph({ heading: HeadingLevel.HEADING_2, children: [new TextRun({ text: t, font: FONT })] });
const BUL = (parts, level = 0) => new Paragraph({
  children: runs(parts), numbering: { reference: "bul", level }, alignment: AlignmentType.JUSTIFIED,
  spacing: { after: 30, line: 240 },
});
const NUM = (parts, ref = "num") => new Paragraph({
  children: runs(parts), numbering: { reference: ref, level: 0 }, alignment: AlignmentType.JUSTIFIED,
  spacing: { after: 30, line: 240 },
});
// Yellow placeholder block for the echo-server owners
const TODO = (text) => new Paragraph({
  children: [new TextRun({ text: `[ECHO SERVER – TO FILL] ${text}`, font: FONT, size: SZ, italics: true, highlight: "yellow" })],
  alignment: AlignmentType.LEFT, spacing: { after: 80, line: 240 },
});
const CODE = (lines) => lines.map((l, i) => new Paragraph({
  children: [new TextRun({ text: l, font: "Courier New", size: 16 })],
  spacing: { after: i === lines.length - 1 ? 100 : 0, line: 220 }, indent: { left: 280 },
  keepNext: i < lines.length - 1, keepLines: true,
  shading: { type: ShadingType.CLEAR, fill: "F3F4F6" },
}));

function pngSize(file) {
  const b = fs.readFileSync(file);
  return { w: b.readUInt32BE(16), h: b.readUInt32BE(20) };
}
function figure(file, caption, widthIn = 6.2) {
  const f = path.join(FIG, file);
  const { w, h } = pngSize(f);
  const wpx = Math.round(widthIn * 96);
  return [
    new Paragraph({
      alignment: AlignmentType.CENTER, spacing: { before: 60, after: 30 }, keepNext: true,
      children: [new ImageRun({ type: "png", data: fs.readFileSync(f), transformation: { width: wpx, height: Math.round(wpx * h / w) },
        altText: { title: caption.slice(0, 40), description: caption, name: file } })],
    }),
    new Paragraph({
      alignment: AlignmentType.CENTER, spacing: { after: 100 },
      children: runs([{ t: caption.split(".")[0] + ".", b: true }, caption.slice(caption.indexOf(".") + 1)], { size: 18 }),
    }),
  ];
}

const border = { style: BorderStyle.SINGLE, size: 4, color: "9CA3AF" };
const borders = { top: border, bottom: border, left: border, right: border };
function table(widths, rows, opt = {}) {
  const total = widths.reduce((a, b) => a + b, 0);
  return new Table({
    width: { size: total, type: WidthType.DXA }, columnWidths: widths,
    rows: rows.map((r, ri) => new TableRow({
      tableHeader: ri === 0,
      children: r.map((c, ci) => new TableCell({
        borders: opt.noBorder ? undefined : borders, width: { size: widths[ci], type: WidthType.DXA },
        shading: ri === 0 && !opt.noHeader ? { type: ShadingType.CLEAR, fill: "E5EAF1" } : undefined,
        margins: { top: 30, bottom: 30, left: 80, right: 80 },
        children: [new Paragraph({
          spacing: { after: 0, line: 228 }, alignment: opt.align?.[ci] || AlignmentType.LEFT,
          children: runs(Array.isArray(c) ? c : [c], { size: opt.size || 18, bold: ri === 0 && !opt.noHeader ? true : undefined }),
        })],
      })),
    })),
  });
}
const tcap = (t) => new Paragraph({ alignment: AlignmentType.CENTER, spacing: { before: 60, after: 40 }, keepNext: true,
  children: runs([{ t: t.split(".")[0] + ".", b: true }, t.slice(t.indexOf(".") + 1)], { size: 18 }) });
const gap = () => new Paragraph({ spacing: { after: 60 }, children: [] });

// ---------- content ----------
const C = [];

// Title block
C.push(new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 60 },
  children: [new TextRun({ text: "Scalable Network I/O: From select to io_uring", font: FONT, size: 32, bold: true })] }));
C.push(new Paragraph({ alignment: AlignmentType.CENTER, spacing: { after: 120 },
  children: runs(["CS 331 Computer Networks · Team T008 · Project ID 13"], { size: 20, italics: true }) }));
const members = [["Chitiveli Hemcharan Varma", "2411095"], ["Gella Naga Sai Krishna", "24110122"], ["Guransh Kaur Saran", "24110127"],
  ["Kowel P Laloo", "24110168"], ["Pallav Biyala", "24110234"]];
C.push(table(Array(5).fill(1805), [members.map((m) => [{ t: m[0], b: true }]), members.map((m) => `Roll No. ${m[1]}`)],
  { noBorder: true, noHeader: true, size: 18, align: Array(5).fill(AlignmentType.CENTER) }));
C.push(gap());

// I. Introduction
C.push(H1("I. Introduction"));
C.push(P("Network servers must handle thousands of concurrent connections with few threads. Linux offers four generations of event-notification interfaces for this: select(2) and poll(2), which scan every watched descriptor on each call; epoll(7), which reports only ready descriptors; and io_uring, which replaces readiness notification with submission and completion queues shared with the kernel [1]–[4]. Which one to use is a recurring engineering decision, and the answer depends on the workload."));
C.push(P("We build two servers – an echo server and an HTTP/1.1 file transfer server – on one shared core and one request-based I/O interface, with exactly one of the four backends linked into each binary. Because the application code is identical across backends, performance differences can be attributed to the event-notification layer. Both servers are benchmarked with an automated, reproducible runner."));
C.push(P("Main contributions: (1) a backend-independent I/O contract (io.h) with four textbook backends; (2) an echo server and an HTTP file server built on it; (3) an experiment runner that pins CPUs, validates every run and archives raw evidence; (4) 360 file-transfer runs across five experiments, with an analysis of when and why the backends differ."));
C.push(TODO("Add 1–2 sentences on the echo-server experiments and their headline result (e.g. which backend scales best with message size or connection count)."));

// II. Problem statement
C.push(H1("II. Problem Statement"));
C.push(P([{ t: "Problem. ", b: true }, "For a single-threaded, event-driven server on Linux, how do select, poll, epoll and io_uring compare in throughput, latency and CPU cost, and how does the answer change with concurrency, payload size, connection reuse, buffer settings, slow clients and TCP options?"]));
C.push(P([{ t: "Inputs: ", b: true }, "backend; number of client connections (1–1000); file size (4 KiB–100 MiB) or echo message size; keep-alive vs Connection: close; chunk and socket-buffer sizes; number of rate-limited slow readers; TCP_NODELAY. ",
  { t: "Outputs: ", b: true }, "throughput (responses/s, MB/s), latency (mean, p99), server CPU per response and server-core utilisation."]));
C.push(P([{ t: "Assumptions and scope: ", b: true }, "one server thread pinned to one core; client and server on one machine over loopback TCP; files served from the page cache; HTTP/1.1 GET only, no TLS. Each backend uses its mechanism in the standard way, with optional features behind flags. Multi-threaded servers, zero-copy sending, real NICs/WANs and TLS are out of scope."]));
C.push(P([{ t: "Gap. ", b: true }, "Many comparisons change the application together with the mechanism, or use micro-benchmarks. We hold the application constant, isolate the event layer, and report CPU cost and server saturation, not only throughput."]));
C.push(TODO("State the echo-server problem precisely: request/response format, message sizes, what is measured (round trips/s, latency), and any echo-specific assumptions."));

// III. Objectives
C.push(H1("III. Project Objectives"));
C.push(NUM("Implement four backends behind one interface; both servers pass the correctness suite on all four backends (file server: 9/9 checks per backend, 36/36 in total).", "obj"));
C.push(NUM("Build a reproducible runner: 5 repetitions per configuration, 10 s warm-up, 30 s measurement, automatic acceptance checks, raw logs archived.", "obj"));
C.push(NUM("Measure file-transfer throughput and latency at 1, 100 and 1000 connections and for 4 KiB, 64 KiB and 1 MiB files (E5a).", "obj"));
C.push(NUM("Quantify the effect of connection reuse (E5b), chunk and socket-buffer sizes (E7), slow readers (E8) and TCP_NODELAY (E9).", "obj"));
C.push(NUM("Explain the results with server CPU time, core utilisation and syscall counters, not only throughput.", "obj"));
C.push(TODO("Add the echo-server objectives (measurable, e.g. message sizes and concurrency levels tested, number of runs)."));

// IV. Architecture
C.push(H1("IV. Architecture and Design"));
C.push(...figure("fig1_architecture.png", "Fig. 1. System architecture. Applications talk only to io.h; one backend is linked into each binary. The runner (right) drives load over loopback and archives results."));
C.push(P([{ t: "Layers. ", b: true }, "Applications implement callbacks and never include epoll.h, poll.h or liburing.h. ", { t: "core.c", i: true }, " holds the connection table, a per-connection carry slot (bytes of the next request), output slots for unsent data, flag validation and statistics counters (dumped on SIGUSR1). ",
  { t: "io.h", i: true }, " is a request-based contract: the app asks for io_read, io_write, io_read_file or io_close, and the module reports completion through callbacks. Readiness backends wait for readiness and then call recv/send; io_uring queues the operation. A request that finishes immediately is queued and delivered at the top of the next loop iteration, which prevents callback recursion and behaves the same in all four modules."]));
C.push(P([{ t: "Backends. ", b: true }, "select rebuilds its fd_sets every iteration and is limited to FD_SETSIZE (1024) descriptors; poll keeps a compact pollfd array; epoll is level-triggered and calls epoll_ctl only when a send is incomplete; io_uring keeps one ACCEPT outstanding, issues a RECV or SEND per connection, and tags user_data with a generation number to reject stale completions."]));
C.push(P([{ t: "File server. ", b: true }, "Each connection runs a small state machine: receive and parse the request (GET, safe file name, Connection header) → send the header → repeat {read a file chunk of up to 64 KiB → write it} → finish, then either wait for the next request (keep-alive) or close. It returns 200, 400, 404 or 405. The header and the file data never share a buffer, because buffer ownership differs between backends."]));
C.push(tcap("Table I. Key design decisions and trade-offs."));
C.push(table([2300, 3363, 3363], [
  ["Decision", "Why", "Trade-off"],
  ["Request-based API (not readiness)", "Fits io_uring natively; same app code on all backends", "Readiness backends must emulate completions (extra staging copy)"],
  ["One backend per binary (compile time)", "No dispatch overhead; clean separation", "Eight binaries to build and test"],
  ["Single thread, pinned core", "Isolates the mechanism; repeatable", "Limits load; a multi-core server would scale further"],
  ["read() + write(), no sendfile", "Backend performs every I/O operation", "Copying dominates large-file cost"],
  ["Sequential keep-alive, no pipelining", "Simple, correct framing", "Less realistic than production HTTP servers"],
]));
C.push(gap());
C.push(TODO("Describe the echo server design: message framing (--msg-size), how it uses io_read / io_write and the carry slot, and how a full echo round trip maps onto the callbacks."));

// V. Implementation
C.push(H1("V. Implementation"));
C.push(P([{ t: "Stack. ", b: true }, "C17 compiled with GCC (GNU Make builds 8 binaries: 2 apps × 4 backends); liburing for io_uring; Python 3 for the experiment runner; wrk with a Lua script for latency percentiles; curl for rate-limited slow readers; matplotlib for analysis. The file server is about 800 lines of C. Code: ",
  new ExternalHyperlink({ link: GH, children: [new TextRun({ text: GH, font: FONT, size: SZ, style: "Hyperlink" })] }), "."]));
C.push(P("Core of the file-server response path (callbacks are always invoked from io_run):", { keepNext: true, after: 40 }));
C.push(...CODE([
  "on_read_done(c, buf, n):  append buf to carry; if a full request is present:",
  "    parse request -> status, file;  hdr = io_get_buf(c);  build header in hdr",
  "    io_write(c, hdr)                          # header first, separate buffer",
  "on_write_done(c):",
  "    if bytes_left > 0:  b = io_get_buf(c);  io_read_file(c, b, min(chunk, left), fd, off)",
  "    elif keep_alive:    io_read(c, buf_size)  # wait for next request",
  "    else:               io_close(c)",
  "on_file_read_done(c, b, n):  off += n;  left -= n;  io_write(c, b, n)",
]));
C.push(P([{ t: "Challenges and solutions. ", b: true }, "(i) Buffer lifetime differs (shared scratch buffer on readiness backends, per-connection buffer on io_uring), so request bytes are copied into the carry slot before the header is built, and a file chunk is read only after the header write completes. (ii) Callback recursion (on_read_done → io_write → on_write_done → …) is avoided with a deferred-completion queue and a zero wait timeout. (iii) Completions arriving after a close are rejected using generation-tagged io_uring user_data; close waits for the in-flight operation. (iv) The first E5b Connection: close campaign was rejected, so all four backends were rerun with --max-conns 300; E8 needed a 90 s window so that 100 MiB downloads could finish."]));
C.push(TODO("Add echo-server implementation notes and challenges (e.g. partial reads/writes, framing large messages, any backend-specific bug you fixed)."));

// VI. Testbed
C.push(H1("VI. Testbed Setup"));
C.push(tcap("Table II. Experimental environment."));
C.push(table([2100, 6926], [
  ["Item", "Value"],
  ["Hardware", "Intel Core i5-10300H (4 cores / 8 threads, 2.5 GHz), ~8 GB RAM available to WSL2"],
  ["OS / software", "Windows 11 + WSL2, Linux 6.18.54-microsoft-standard-WSL2, glibc 2.43, Python 3.14.4, GCC (C17), liburing, wrk"],
  ["Topology", "Single host, loopback 127.0.0.1. Server pinned to CPU 1; wrk (4 threads) pinned to CPUs 2–5"],
  ["Kernel limits", "net.core.somaxconn = 4096; open files 10,240; kernel.io_uring_disabled = 0"],
  ["Workloads", "Deterministic fixtures of 4 KiB, 64 KiB, 1 MiB, 4 MiB and 100 MiB (make bench-fixtures)"],
  ["Timing", "10 s warm-up, 30 s measured (90 s in E8), 10 s cool-down; 5 repetitions; configuration order randomised"],
]));
C.push(tcap("Table III. File-transfer experiments (all on all four backends)."));
C.push(table([700, 3400, 3626, 1300], [
  ["Exp.", "Varied", "Fixed", "Runs"],
  ["E5a", "Connections 1/100/1000 (4 KiB); 64 KiB at 10; 1 MiB at 100", "Keep-alive, 64 KiB chunks", "100"],
  ["E5b", "Connection: close vs keep-alive", "4 KiB, 100 conns, --max-conns 300", "40"],
  ["E7", "Chunk 16 KiB; SO_RCVBUF 512 KiB; SO_SNDBUF 64 KiB", "4 MiB, 100 conns, app buffer 256 KiB", "80"],
  ["E8", "0 / 10 / 100 slow readers (100 KiB/s)", "100 MiB, 100 fast wrk connections", "60"],
  ["E9", "TCP_NODELAY on / off", "4 KiB and 64 KiB, 100 conns", "80"],
]));
C.push(gap());
C.push(TODO("Add the echo-server workloads and parameters (message sizes, connection counts, client tool and command, number of runs) as rows or a separate table."));

// VII. Evaluation
C.push(H1("VII. Evaluation"));
C.push(P([{ t: "Method. ", b: true }, "Each point is the median of 5 accepted runs; whiskers show the inter-quartile range. 357 of 360 file-transfer runs were accepted. Throughput is measured by wrk; latency is wrk's mean time to the last byte. Because wrk is closed-loop (each connection sends its next request as soon as the previous one completes), mean latency ≈ connections ÷ throughput (Little's law [5]). Server CPU and core utilisation come from /proc."]));

C.push(H2("A. Echo server"));
C.push(TODO("Present the echo-server results here: one figure with 2–3 panels (e.g. throughput vs connections, latency vs message size) plus 150–250 words of discussion. Suggested budget: about 1 page including the figure. Use the same colours as the file-transfer figures (select blue, poll orange, epoll green, io_uring yellow) and the same median + IQR convention."));

C.push(H2("B. File transfer: concurrency and file size (E5a)"));
C.push(...figure("fig2_e5a.png", "Fig. 2. E5a. (a) Throughput and (b) mean latency vs concurrency for a 4 KiB file; (c) throughput vs file size. Medians of 5 runs; whiskers show the IQR."));
C.push(P("From 1 to 100 connections throughput rises 37% (4.3k → 5.9k req/s) on every backend; at 1000 connections it falls 7–15%, and poll falls most (4,989 vs 5,484 req/s for epoll) because it rebuilds and rescans its 1000-entry pollfd array three times per loop in user space. Latency follows Little's law (100/5,925 ≈ 17 ms; 1000/5,484 ≈ 182 ms). The exception is io_uring at 1000 connections: the same throughput as epoll, but a mean of 818 ms and a p99 of 13.5 s in all 5 runs – some connections are starved for seconds, a fairness problem. Small files are request-bound (~24.5 MB/s at 4 KiB); 64 KiB and 1 MiB files reach the same ~170 MB/s ceiling on all backends. Note that the 64 KiB point used 10 connections."));

C.push(H2("C. Connection reuse and buffer sizes (E5b, E7)"));
C.push(...figure("fig3_e5b_e7.png", "Fig. 3. (a, b) E5b: Connection: close vs keep-alive, 4 KiB, 100 connections. (c, d) E7: buffer settings, 4 MiB file, 100 connections."));
C.push(P([{ t: "E5b. ", b: true }, "Keep-alive measured +5 to +8% for poll, epoll and io_uring and 0% for select. io_uring gains most, matching its 15% higher CPU per response when every response opens a new connection. However, the keep-alive half ran 7–12% below the identical E5a configuration in a separate session, so variation between sessions is larger than the effect and the throughput result is inconclusive. On loopback a handshake is almost free; on a real network each new connection adds at least one RTT."]));
C.push(P([{ t: "E7. ", b: true }, "Only the chunk size matters: 16 KiB chunks cut throughput 41% (169 → 99 MB/s) and raise latency per 4 MiB file from 2.5 s to 4.2 s, with 4× the read/write cycles and 2.4× the server CPU per file. The server core is still only ~34% busy, so the loss most likely comes from the client and loopback handling 4× as many smaller writes. SO_RCVBUF has no effect (the server only receives a short GET) and a 64 KiB SO_SNDBUF still holds a whole chunk."]));

C.push(H2("D. Slow readers and TCP_NODELAY (E8, E9)"));
C.push(...figure("fig4_e8_e9.png", "Fig. 4. (a, b) E8: 100 fast clients downloading 100 MiB with 0/10/100 slow readers (100 KiB/s each); at 100 slow readers select, poll and epoll have n = 4. (c, d) E9: TCP_NODELAY on/off, 100 connections."));
C.push(P([{ t: "E8. ", b: true }, "Ten slow readers have no measurable effect. With 100, fast-client throughput falls 7–11% and each 100 MiB download takes 67–70 s instead of ~62 s. All four servers are non-blocking: a slow reader's write simply waits (EAGAIN or a pending SQE) while the loop serves others, and 100 × 100 KiB/s ≈ 10 MB/s is close to what the fast clients lose. Run 3 at 100 slow readers was rejected for select, poll and epoll (no download finished within 90 s), so these medians are optimistic."]));
C.push(P([{ t: "E9. ", b: true }, "Disabling TCP_NODELAY cuts 4 KiB throughput by 62% (5.9k → 2.27k req/s) and raises latency 2.6× on every backend. The server sends the header and body as two writes; with Nagle's algorithm [6] the small body waits for the header's ACK, which the client delays by ~40 ms [7]. Check: 100 connections ÷ 44 ms ≈ 2,270 req/s, as measured. At 64 KiB only a 53-byte tail is held and the preceding full segment triggers an immediate ACK, so there is no effect."]));

C.push(H2("E. Why the backends look alike, and where they differ"));
C.push(...figure("fig5_analysis.png", "Fig. 5. (a) Busy share of the server's CPU core during the measured window (median of all backends). (b) Server CPU time per response relative to epoll (0 = same as epoll; lower is cheaper)."));
C.push(P("Fig. 5(a) explains the near-identical throughput: the server core is 4–48% busy in every test, so the limit lies outside the server – most likely wrk, which had only two physical cores, plus the WSL2 loopback path. A backend that saves CPU just leaves the server idle longer. In addition, the backend-specific wait step is tiny: at 100 connections the server waits ~180 times/s and handles ~33 responses per wakeup, against ~17,700 read/write calls/s that are identical code on every backend (Table IV)."));
C.push(tcap("Table IV. Wait calls vs data syscalls (server counters, medians)."));
C.push(table([2600, 2142, 2142, 2142], [
  ["Configuration", "Wait calls/s", "Responses per wakeup", "read + write calls/s"],
  ["4 KiB, 100 conns", "~180", "33", "~17,700"],
  ["4 KiB, 1000 conns", "18–55", "97–282", "~16,000"],
  ["4 MiB, 100 conns", "29–54", "~1", "~2,660"],
], { align: [AlignmentType.LEFT, AlignmentType.CENTER, AlignmentType.CENTER, AlignmentType.CENTER] }));
C.push(gap());
C.push(P("The backends do differ in CPU cost (Fig. 5(b)): poll needs 27% more CPU per response at 1000 connections; io_uring needs 15% more under Connection: close (per-connection setup) and 12–13% less on large files. Most of that last saving is an implementation effect: our readiness backends pread each chunk into a staging slot and then memcpy it into the send buffer, while io_uring reads straight into the send buffer. On a saturated server these CPU differences would turn into throughput differences."));
C.push(TODO("Optional (≈ 0.25 page): compare echo vs file transfer – e.g. whether backend differences are larger for the echo server, where per-message work is small and the wait step is a bigger share of the work."));

// VIII. Summary
C.push(H1("VIII. Summary"));
C.push(P("We compared select, poll, epoll and io_uring under identical application code for an HTTP file server. In throughput and mean latency the four backends are nearly indistinguishable, because the server was never saturated and the backend-specific step is about 1% of its work; copying data through TCP dominates. The backends do differ in CPU cost (poll +27% at 1000 connections; io_uring −12% on large files, mostly from a copy our readiness backends make) and in tail latency (io_uring p99 13.5 s at 1000 connections). TCP and buffer settings matter far more than the backend: TCP_NODELAY off costs 62% of small-file throughput, and 16 KiB chunks cost 41%."));
C.push(TODO("Add 2–3 sentences summarising the echo-server findings and how they compare with the file-transfer findings."));
C.push(P([{ t: "Limitations and implementation flaws. ", b: true }, "(i) The server was never saturated and client CPU was not recorded; wrk ran on two physical cores. (ii) Readiness backends make an extra user-space copy of every file chunk, and io_uring uses only basic features (synchronous pread, no multishot, provided buffers or SQPOLL), so io_uring is not shown at its best. (iii) The io_uring starvation tail at 1000 connections is unexplained. (iv) Binaries were built without an optimisation flag (-O0). (v) Closed-loop load only; WSL2 loopback (~64 KiB segments, no real RTT); E5b halves ran in separate sessions; E5a is not a full grid and E7 never varied the app buffer; E8's window was short; 5 runs and no significance tests."]));
C.push(P([{ t: "Future work. ", b: true }, "Cap the server with a cgroup CPU quota so it becomes the bottleneck and record client CPU; build with -O2; remove the staging copy and add sendfile/splice and a single writev for header and body; use io_uring's advanced features and fix its fairness problem; add a fixed-rate (open-loop) load test; rerun E5b in one session; and repeat key experiments over a real network with more repetitions and confidence intervals."]));

// IX. Reproducibility
C.push(H1("IX. Reproducibility"));
C.push(P(["Repository: ", new ExternalHyperlink({ link: GH, children: [new TextRun({ text: GH, font: FONT, size: SZ, style: "Hyperlink" })] }),
  " (branch project2). Requirements: Linux or WSL2 with GCC, GNU Make, pkg-config, liburing-dev, Python 3, wrk and curl. From the repository root:"], { keepNext: true }));
C.push(...CODE([
  "sudo apt install build-essential pkg-config liburing-dev python3 curl wrk",
  "make && make test                      # build 8 binaries, run correctness suite",
  "make bench-fixtures                    # deterministic test files",
  "python3 code/tools/File-Transfer-Server/run_experiment \\",
  "    code/tools/File-Transfer-Server/configs/filesrv-e5a-size-concurrency.json",
  "# also: filesrv-e5b-*-c300.json, filesrv-e7-*.json, filesrv-e8-*.json, filesrv-e9-*.json",
  "python3 report/file-transfer/make_figures.py         # figures + data.json",
  "python3 report/file-transfer/make_report_figures.py  # figures used in this report",
]));
C.push(P("Raw results of every run (accepted and rejected) are archived under code/results/File Transfer Server/. The detailed analysis is in report/file-transfer/README.md."));
C.push(TODO("Add the commands and config files used to run and plot the echo-server experiments."));

// References
C.push(H1("References"));
const refs = [
  "J. Axboe, “Efficient IO with io_uring,” 2019. [Online]. Available: https://kernel.dk/io_uring.pdf",
  "Linux man-pages project, “epoll(7), select(2), poll(2),” The Linux Programming Interface manual pages.",
  "G. Banga, J. C. Mogul, and P. Druschel, “A scalable and explicit event delivery mechanism for UNIX,” in Proc. USENIX Annu. Tech. Conf., Monterey, CA, USA, 1999, pp. 253–265.",
  "D. Kegel, “The C10K problem.” [Online]. Available: http://www.kegel.com/c10k.html",
  "J. D. C. Little, “A proof for the queuing formula: L = λW,” Operations Research, vol. 9, no. 3, pp. 383–387, 1961.",
  "J. Nagle, “Congestion control in IP/TCP internetworks,” RFC 896, Jan. 1984.",
  "R. Braden, Ed., “Requirements for Internet hosts – communication layers,” RFC 1122, Oct. 1989.",
  "R. Fielding, M. Nottingham, and J. Reschke, Eds., “HTTP/1.1,” RFC 9112, Jun. 2022.",
  "W. Glozer, “wrk – a HTTP benchmarking tool.” [Online]. Available: https://github.com/wg/wrk",
];
refs.forEach((r, i) => C.push(new Paragraph({ spacing: { after: 30 }, indent: { left: 400, hanging: 400 },
  children: runs([`[${i + 1}]\t${r}`], { size: 18 }) })));
C.push(TODO("Add any references used for the echo-server part."));

// Appendix A
C.push(new Paragraph({ pageBreakBefore: true, heading: HeadingLevel.HEADING_1, children: [new TextRun({ text: "Appendix A: Team Work Distribution and Member Rating", font: FONT })] }));
C.push(H2("A.1 Work Distribution"));
C.push(P("Ratings are agreed by the whole team and must be unique (no ties), on a scale of 1–10 (10 = highest contribution)."));
C.push(table([500, 2600, 1300, 3426, 1200], [
  ["#", "Member Name", "Roll No.", "Tasks Done", "Rating (1–10, unique)"],
  ["1", "Chitiveli Hemcharan Varma", "2411095", [{ t: "Benchmarking (from README) – [fill in]", hl: true }], [{ t: "[ ]", hl: true }]],
  ["2", "Gella Naga Sai Krishna", "24110122", [{ t: "io_uring backend; file-transfer server evaluation, analysis, figures and report – [confirm]", hl: true }], [{ t: "[ ]", hl: true }]],
  ["3", "Guransh Kaur Saran", "24110127", [{ t: "select backend – [fill in]", hl: true }], [{ t: "[ ]", hl: true }]],
  ["4", "Kowel P Laloo", "24110168", [{ t: "poll backend – [fill in]", hl: true }], [{ t: "[ ]", hl: true }]],
  ["5", "Pallav Biyala", "24110234", [{ t: "epoll backend – [fill in]", hl: true }], [{ t: "[ ]", hl: true }]],
]));

// Appendix B
C.push(new Paragraph({ heading: HeadingLevel.HEADING_1, spacing: { before: 300 }, children: [new TextRun({ text: "Appendix B: AI Usage Details", font: FONT })] }));
C.push(H2("B.1 Prompts"));
C.push(P("The most significant AI uses are listed below. Prompts and responses are shared in the repository's AI-Logs folder."));
C.push(table([500, 3000, 5526], [
  ["#", "AI Tool", "Purpose"],
  ["1", "Claude (Claude Code, Claude Opus 5.5)", "Analysing the file-transfer benchmark data, producing figures, and drafting the file-transfer report and slides; all numbers were checked against the raw run logs"],
  ["2", [{ t: "[fill in]", hl: true }], [{ t: "[fill in – e.g. echo-server or backend development uses]", hl: true }]],
  ["3", [{ t: "[fill in]", hl: true }], [{ t: "[fill in]", hl: true }]],
  ["4", [{ t: "[fill in]", hl: true }], [{ t: "[fill in]", hl: true }]],
]));

// ---------- document ----------
const doc = new Document({
  creator: "CS331 Team T008",
  title: "Scalable Network I/O: From select to io_uring",
  styles: {
    default: { document: { run: { font: FONT, size: SZ } } },
    paragraphStyles: [
      { id: "Heading1", name: "Heading 1", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { font: FONT, size: 24, bold: true, color: "14283C" }, paragraph: { spacing: { before: 160, after: 60 }, keepNext: true, outlineLevel: 0 } },
      { id: "Heading2", name: "Heading 2", basedOn: "Normal", next: "Normal", quickFormat: true,
        run: { font: FONT, size: 21, bold: true, italics: true, color: "14283C" }, paragraph: { spacing: { before: 100, after: 40 }, keepNext: true, outlineLevel: 1 } },
    ],
  },
  numbering: {
    config: [
      { reference: "bul", levels: [{ level: 0, format: LevelFormat.BULLET, text: "•", alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 360, hanging: 240 } } } }] },
      { reference: "num", levels: [{ level: 0, format: LevelFormat.DECIMAL, text: "%1.", alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 360, hanging: 300 } } } }] },
      { reference: "obj", levels: [{ level: 0, format: LevelFormat.DECIMAL, text: "O%1.", alignment: AlignmentType.LEFT, style: { paragraph: { indent: { left: 480, hanging: 420 } } } }] },
    ],
  },
  sections: [{
    properties: { page: { size: { width: 11906, height: 16838 }, margin: { top: 1440, right: 1440, bottom: 1440, left: 1440 } } },
    footers: { default: new Footer({ children: [new Paragraph({ alignment: AlignmentType.CENTER,
      children: [new TextRun({ children: [PageNumber.CURRENT], font: FONT, size: 18 })] })] }) },
    children: C,
  }],
});

Packer.toBuffer(doc).then((buf) => { fs.writeFileSync(OUT, buf); console.log("wrote", OUT); });
