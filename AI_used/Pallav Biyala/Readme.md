# AI Documentation:
---
### 1) Tools Used:
The AI tools used by me in this project are:
- Gemini
- ChatGPT
- Claude

The primary use was to understand networking and I/O multiplexing concepts, review implementation decisions, identify possible issues in the code, and improve the clarity of the project documentation.

---

#### 2) Prompts:
The link to my AI chat for all three AIs is given below:

- `Gemini`: https://share.gemini.google/zCBlE6R2HfoJ
- `ChatGPT`: https://chatgpt.com/share/6aa234f9-3790-83e8-8b83-b4093ef4505f
- `Claude`: https://claude.ai/share/4a08daa8-321a-4d31-b480-f32f2ea5477c

### 3) Thought Process:
AI was used as a supporting tool rather than as a replacement for the team's implementation work.

The general workflow was:

- Understand the networking concept with the help of ChatGPT.
- Develop the implementation based on the team's understanding of the required mechanism.
- Share the implementation with ChatGPT for code review and conceptual checking.
- Identify possible bugs, limitations, or edge cases.
- Test the implementation locally.
- Discuss the observed behaviour and possible causes with AI.
- Make implementation decisions based on the team's judgement and project requirements.
- Document the mechanism and implementation in the project report.

AI was particularly useful for explaining concepts that were initially unclear and for acting as an additional code-review perspective.

--

#### 4) Step by Step Details:
# AI Usage Disclosure — Epoll Server Component

| Stage | What was reviewed | What AI caught / contributed |
|---|---|---|
| 1. Foundation | `network_utils.c` (socket creation, `SO_REUSEADDR`, `fcntl` non-blocking) | Confirmed correctness; flagged that accepted client sockets (not just the listener) would also need to be set non-blocking later. |
| 2. First epoll draft | `server.c` v1 — `epoll_create1`, `epoll_ctl`, accept loop, `EAGAIN` draining | Flagged: (1) unbounded partial-write data loss on `send()`, (2) starvation risk from combining level-triggered epoll with a drain-until-`EAGAIN` loop, (3) `epoll_ctl` failure in accept loop wrongly using `break` instead of `continue`, (4) fd-lifecycle ownership mixed into `register_socket_epoll`, (5) uninitialized `epoll_event` struct padding. |
| 3. Bug-fix pass | v2 after applying above fixes | Caught a new compile error (`ev` used in `main()` but declared only inside a different function) and a missing `#include <string.h>`, introduced while applying the previous round's fixes. |
| 4. Level-triggered decision | v3 | Confirmed the switch to level-triggered epoll (dropping the inner drain loop) correctly resolved the starvation issue. |
| 5. Partial-write design | Conceptual discussion, no code yet | Explained *why* `send()` can partially fail under `EAGAIN`, and outlined the three components needed to fix it properly: per-client state struct, a pointer-based epoll lookup (`epoll_data.ptr`) instead of an fd-indexed array, and an `EPOLLOUT`-driven flush routine. |
| 6. `client_state` struct + `flush_outbound_buffer` | v4 | Verified the `memmove`-based buffer-shifting logic, the `EPOLL_CTL_MOD` re-arming logic, and the listener/client disambiguation via `data.ptr == NULL`. Caught a real bug: `&&` used instead of `&` when checking `revents` bitmasks (meant every event ran every branch, regardless of what actually fired). |
| 7. Stress testing | N/A — new test artifacts | AI wrote a Python stress-test harness (concurrency test, slow-reader test designed to force `EAGAIN`, and a connection-churn test) and ran it against the compiled server. This surfaced a real, previously undetected bug: the fixed-size `out_buf` (64 KB) could overflow under a large/slow-reading client, at which point the server disconnected the client rather than corrupting or silently dropping data. |
| 8. Design decision on buffer overflow | Team discussion | Team decided to treat the overflow-disconnect behavior as a **documented limitation** rather than implementing full read-side backpressure, and to keep `OUT_BUF_CAP` at 64 KB (not 1 MB) specifically to keep benchmarking comparable against select/poll/io_uring. |
| 9. Poll implementation review | Teammate's `server_poll.c` | Reviewed for a shared review pass. Found one materially serious bug: accepted client sockets were never set non-blocking, meaning a slow client's `write()` would block the entire single-threaded event loop (not just drop data, as the code's own comment assumed). Also flagged a minor `EINTR` handling gap on `read()`. |
| 10. `TCP_NODELAY` | Cross-cutting change | Discussed why Nagle's algorithm + delayed ACK can distort request/response latency benchmarks, and added a shared `set_tcp_nodelay()` helper to `network_utils.c`/`.h` so every I/O-model implementation applies it identically (for benchmark fairness). |
| 11. Report writing | Introduction, socket lifecycle, blocking/non-blocking I/O sections | AI reviewed drafts written independently for technical accuracy (e.g., confirming that blocking `read()` correctly puts *the thread*, not "the server," to sleep) and suggested small clarity/structure edits. No section was AI-generated from scratch — all text was human-authored, then checked. |

---

### 5) Summary

AI was used as a **reviewer and debugging aid**, not as a code author. Every bug it identified is traceable to a specific line the student wrote; the fix was implemented by the student in the next iteration and re-checked. The one piece of AI-authored artifact in this workflow is the Python stress-test script (Section 3, stage 7), which was written by AI to validate the human-written C server, and its results (pass/fail per test) directly informed a real design decision (Section 3, stage 8) that the team made together.

