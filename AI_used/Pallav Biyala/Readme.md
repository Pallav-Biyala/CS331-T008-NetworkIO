# AI Use & Collaboration Disclosure

## 1. Tools Used
The following AI tools were utilized during the research, implementation, and analysis phases of this project:
* **Google Gemini**
* **OpenAI ChatGPT**
* **Anthropic Claude**

Primary usage focused on understanding networking and I/O multiplexing concepts, reviewing architecture decisions, identifying edge-case bugs, and refining project documentation.

---

## 2. Prompts & Conversation History
Transparency links to the full AI interaction logs for this project:
* **Gemini Share Link:** [https://share.gemini.google/zCBlE6R2HfoJ](https://share.gemini.google/zCBlE6R2HfoJ)
* **ChatGPT Share Link:** [https://chatgpt.com/share/6aa234f9-3790-83e8-8b83-b4093ef4505f](https://chatgpt.com/share/6aa234f9-3790-83e8-8b83-b4093ef4505f)
* **Claude Share Link:** [https://claude.ai/share/4a08daa8-321a-4d31-b480-f32f2ea5477c](https://claude.ai/share/4a08daa8-321a-4d31-b480-f32f2ea5477c)
(I thought since my team modified my AI documentation and removed these links, so I thought these wont be required so I unfortunately deleted these chats)
---

## 3. Methodology & Thought Process
AI was leveraged strictly as an interactive tutor and secondary code reviewer—not as a substitute for human development and system architecture decisions.

**Standard Development Workflow:**
1. **Conceptual Mastery:** Explored low-level networking and kernel primitives via AI interaction.
2. **Implementation:** Developed the C server implementations independently based on core understanding.
3. **Static Review:** Shared original implementations with AI to perform static code analysis and conceptual checks.
4. **Edge Case Analysis:** Identified potential deadlocks, buffer overflows, and state handling errors.
5. **Local Validation:** Executed unit tests and stress tests locally.
6. **Iterative Refinement:** Discussed observed runtime behavior with AI to analyze root causes.
7. **Human Judgment:** Made all final architecture and code decisions based on engineering trade-offs and project requirements.
8. **Documentation:** Documented findings, mechanisms, and benchmarks in the final report.

---

## 4. Step-by-Step Breakdown

### Stage 1 — System Scale & Problem Definition
Constructed the theoretical foundation of high-concurrency network servers:
$$\text{Blocking I/O} \longrightarrow \text{Non-Blocking I/O} \longrightarrow \text{I/O Multiplexing} \longrightarrow (\text{select} \to \text{poll} \to \text{epoll} \to \text{io\_uring})$$

### Stage 2 — Epoll Kernel Architecture Deep-Dive
Analyzed low-level system calls and kernel-space primitives:
* `epoll_create1()`, `epoll_ctl()`, and `epoll_wait()` lifecycles
* Interest set registration vs. ready list notifications
* Handling non-blocking sockets with `EPOLLIN` and `EPOLLOUT` bitmasks
* Managing `EAGAIN` / `EWOULDBLOCK` signals and partial write offsets

### Stage 3 — Code Review & Edge-Case Identification
Conducted review passes on custom server code to handle low-level edge cases:
* Managing client-side non-blocking sockets and output buffer queues
* Enabling `EPOLLOUT` triggers dynamically when unsent buffer data remains
* Connection teardown protocols and `EPOLLERR` / `EPOLLHUP` handling
* Analyzing file descriptor allocation, reuse, and lifecycle by the kernel
* Level-Triggered (`LT`) vs. Edge-Triggered (`ET`) operational nuances

### Stage 4 — Testing & Validation
Verified server behavior under concurrent connections using utilities like `netcat` (`nc`):
* Verified simultaneous multi-client connection persistence
* Confirmed non-blocking echo responses and read/write buffer accuracy
* Validated socket recycling and graceful disconnect behavior
* Analyzed stress-testing results under high message volume payloads

### Stage 5 — Documentation & Technical Communication
Refined explanations in the final project report for clarity and precision:
* Formulated real-world analogies (e.g., restaurant polling vs. ready notification bells) to illustrate $O(N)$ scanning vs. $O(1)$ event loops.
* Corrected technically misleading terminology while retaining human-authored structure.

### Stage 6 — Benchmark Interpretation & Metric Analysis
Evaluated performance anomalies where `poll` and `epoll` exhibited unexpected throughput or latency behavior:
* Separated theoretical algorithmic complexity $O(1)$ vs. $O(N)$ from practical runtime metrics.
* Evaluated environmental variables: workload characteristics, virtualization context, load-generator behavior, and hardware constraints.
* Avoided blanket claims, ensuring claims reflected actual empirical data rather than assumptions.

---

## 5. Scope & Summary of AI Involvement
AI served exclusively as a tool for:
* **Conceptual Learning & System Architecture Analysis**
* **Technical & System Call Clarifications**
* **Code Review & Static Safety Checks**
* **Edge-Case & Failure Mode Analysis**
* **Report Proofreading & Clarity Improvements**
* **Benchmark Result Interpretation**

All core logic, code execution, integration, and final benchmarking validation remain the original, verified work of the project team.
