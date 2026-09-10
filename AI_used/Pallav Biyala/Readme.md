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

---

### 3) Thought Process:
AI was used as a supporting tool rather than as a replacement for the team's implementation work.

The general workflow was:

- Understand the networking concept with the help of the AIs.
- Develop the implementation based on the team's understanding of the required mechanism.
- Share the implementation with AIs for code review and conceptual checking.
- Identify possible bugs, limitations, or edge cases.
- Test the implementation locally.
- Discuss the observed behaviour and possible causes with AI.
- Make implementation decisions based on the team's judgement and project requirements.
- Document the mechanism and implementation in the project report.

AI was particularly useful for explaining concepts that were initially unclear and for acting as an additional code-review perspective.

---

### 4) Step by Step Details:

### Stage 1 — Understanding the Problem

AI was used to understand the scalability problem in network servers:

Blocking I/O
      ↓
Non-blocking I/O
      ↓
How do we efficiently identify ready sockets?
      ↓
I/O Multiplexing
      ↓
select → poll → epoll → io_uring

This helped establish the conceptual progression used in the report.

### Stage 2 — Understanding Epoll

For the epoll implementation, AI was used extensively to understand:
<ul>
<li>epoll instances</li>
<li>file descriptors</li>
<li>epoll_create1()</li>
<li>epoll_ctl()</li>
<li>epoll_wait()</li>
<li>readiness notifications</li>
<li>EPOLLIN</li>
<li>EPOLLOUT</li>
<li>non-blocking sockets</li>
<li>EAGAIN/EWOULDBLOCK</li>
<li>partial writes</li>
</ul>
The concepts were discussed interactively rather than simply copying a finished implementation.

### Stage 3 — Code Review

The epoll implementation was reviewed with AI after development.

The review identified and discussed issues such as:
<ul>
<li>handling non-blocking client sockets
<li>partial send() operations
<li>maintaining an output buffer
<li>enabling EPOLLOUT when unsent data remains
<li>handling disconnects
<li>handling EPOLLERR/EPOLLHUP
<li>understanding why closing a file descriptor can result in the operating system reusing that FD number
<li>distinguishing level-triggered behaviour from edge-triggered behaviour
</ul>

### Stage 4 — Testing

The implementation was tested using local TCP clients such as nc.

Multiple client connections were tested to verify that:
<ul>
<li>multiple client sockets could be handled simultaneously;
<li>data received from clients was echoed back;
<li>clients could disconnect and reconnect;
<li>file descriptors could be reused by the operating system.
</ul>
A separate stress-test script was also discussed to send a larger amount of data and verify that the echoed response was received.

### Stage 5 — Report Preparation

AI was used to review the report's explanations and make them clearer while retaining the team's own structure and understanding.

For example, the restaurant/table-and-bell analogy was used to explain the difference between repeatedly checking every FD and receiving notifications for ready FDs.

AI also helped identify technically misleading wording and suggested corrections while the final wording and content were decided by the team.

### Stage 6 — Benchmark Interpretation

AI was consulted while interpreting benchmark results, particularly when the measured performance of poll and epoll did not always follow the assumption that epoll must be faster.

The discussion helped distinguish between:
<ul>
<li>theoretical scalability,
<li>actual measured performance,
<li>workload dependence,
<li>virtualization effects,
<li>implementation differences,
<li>and benchmark variability.
</ul>
The team therefore avoided making the unsupported claim that newer mechanisms are always faster.

---

### 5) Role of AI in the Final Project

AI was used primarily for:
<ul>
<li>Conceptual learning
<li>Technical clarification
<li>Code review
<li>Debugging assistance
<li>Testing discussion
<li>Edge-case analysis
<li>Report editing
<li>Benchmark-result interpretation
</ul>
The team remained responsible for writing, testing, integrating, and validating the project implementation.

AI-generated suggestions were reviewed by the team before being incorporated into the project.

---
