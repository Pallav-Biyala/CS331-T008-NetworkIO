# AI Usage Documentation

## 1. Tools Used
**ChatGPT** was used as an AI-assisted learning, code-review, debugging, and documentation tool during the development of the `epoll` component. The primary use was to understand networking and I/O multiplexing concepts, review implementation decisions, identify possible issues in the code, and improve the clarity of the project documentation.

## 2. How AI Was Integrated Into the Workflow
AI was used as a supporting tool rather than as a replacement for the team's implementation work. The general workflow was:

1. Understand the networking concept with the help of ChatGPT.
2. Develop the implementation based on the team's understanding of the required mechanism.
3. Share the implementation with ChatGPT for code review and conceptual checking.
4. Identify possible bugs, limitations, or edge cases.
5. Test the implementation locally.
6. Discuss the observed behaviour and possible causes with AI.
7. Make implementation decisions based on the team's judgement and project requirements.
8. Document the mechanism and implementation in the project report.

AI was particularly useful for explaining concepts that were initially unclear and for acting as an additional code-review perspective.

## 3. Step-by-Step AI Contribution

### Stage 1 — Understanding the Problem
AI was used to understand the scalability problem in network servers, establishing the conceptual progression:
Blocking I/O → Non-blocking I/O → How do we efficiently identify ready sockets? → I/O Multiplexing → `select` → `poll` → `epoll` → `io_uring`.

| Prompt | Purpose |
| :--- | :--- |
| "Explain blocking and non-blocking I/O." | Established the base distinction driving the whole project. |
| "Why is non-blocking I/O useful for handling multiple clients?" | Motivated the need for I/O multiplexing. |
| "Explain select, poll, epoll and io_uring and how they differ." | Framed the overall `select → poll → epoll → io_uring` progression used in the report. |

### Stage 2 — Understanding Epoll
For the epoll implementation, AI was used extensively to understand epoll instances, file descriptors, readiness notifications, and non-blocking I/O mechanics. The concepts were discussed interactively rather than simply copying a finished implementation.

| Prompt | Purpose |
| :--- | :--- |
| "What exactly does epoll do?" | Built the base conceptual model of epoll before implementation. |
| "Explain epoll_create1(), epoll_ctl() and epoll_wait()." | Clarified the three core epoll syscalls. |
| "What does EPOLLIN mean?" / "What does EPOLLOUT mean?" | Clarified readiness-event flags. |
| "Explain level-triggered epoll." | Established why the implementation uses level-triggered (not edge-triggered) mode. |
| "What is EAGAIN/EWOULDBLOCK and why does it occur with non-blocking sockets?" | Explained the error code driving the non-blocking read/write handling. |

### Stage 3 — Code Review
The epoll implementation was reviewed with AI after development, surfacing issues such as non-blocking client sockets, partial writes, output buffering, `EPOLLOUT` re-arming, disconnect handling, and level- vs. edge-triggered behaviour.

| Prompt | Purpose |
| :--- | :--- |
| "Check this epoll implementation for issues." | General review pass on the implementation. |
| "Is the accepted client socket non-blocking?" | Verified sockets were configured correctly. |
| "Why do we need an output buffer here?" | Clarified the need to buffer unsent data. |
| "What happens if send() only sends part of the data?" | Surfaced the partial-write problem. |
| "Explain whether this implementation correctly handles partial writes." | Confirmed the fix handled partial writes correctly. |
| "Is this limitation acceptable for the scope of our project?" | Sanity-checked scope decisions against project requirements. |

### Stage 4 — Testing
The implementation was tested using local TCP clients such as `nc`, verifying simultaneous client handling, correct echo behaviour, disconnect/reconnect handling, and FD reuse by the OS. A separate stress-test script was also discussed.

| Prompt | Purpose |
| :--- | :--- |
| (Discussion-based — used to interpret test behaviour such as FD reuse after a client disconnects, and to design a Python stress-test script.) | Interpreted observed runtime behaviour and validated echoed data under load. |

### Stage 5 — Report Preparation
AI was used to review the report's explanations and make them clearer while retaining the team's own structure and understanding — including reviewing wording, structure, and the epoll explanation (e.g. a restaurant/table-and-bell analogy for polling vs. notification).

| Prompt | Purpose |
| :--- | :--- |
| "Review this section of the report for technical correctness." | Checked explanations for accuracy. |
| "Can you make this explanation clearer?" | Improved wording and organization of the epoll write-up. |

### Stage 6 — Benchmark Interpretation
AI was consulted while interpreting benchmark results, particularly when measured `poll`/`epoll` performance did not always follow the assumption that `epoll` must be faster — helping distinguish theoretical scalability from actual measured performance, workload dependence, virtualization effects, and benchmark variability.

| Prompt | Purpose |
| :--- | :--- |
| "Why might poll and epoll show similar performance here instead of epoll being clearly faster?" | Avoided the unsupported claim that newer mechanisms are always faster. |

## 4. Role of AI in the Final Project
AI was used primarily for:
- Conceptual learning
- Technical clarification
- Code review
- Debugging assistance
- Testing discussion
- Edge-case analysis
- Report editing
- Benchmark-result interpretation

The team remained responsible for writing, testing, integrating, and validating the project implementation. AI-generated suggestions were reviewed by the team before being incorporated into the project.
