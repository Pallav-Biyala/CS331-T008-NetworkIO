# AI Usage Documentation

This document outlines how artificial intelligence tools were integrated into my specific portion of the team project. 

**Team Division of Labor Context:**
* **Guransh:** Implementation of the `select` single-threaded TCP echo server and presentation slides.
* **My Role:** Core conceptual research, kernel-level mechanics, and the implementation of the `poll` single-threaded TCP echo server and final analysis.
* **Sai Krishna:** Implementation of the modern asynchronous engine `io_uring`.
* **Hemcharan:** Load generation (tcpkali/wrk), benchmarking, profiling, and final analysis.
* **Pallav:** Implementation of the modern asynchronous engine `epoll`.

For my deliverables, my primary use of AI was as a **conceptual tutor and architectural sounding board** to deeply understand Linux network stack mechanics, legacy multiplexing, and kernel/user-space transitions before implementing the code, as well as serving as an advanced **analytical tool for root-cause debugging** during the benchmarking phase.

## 1. Tools Used
I utilized a multi-LLM approach to separate foundational learning, conceptual verification, code generation, and empirical analysis:
* **Google NotebookLM:** Served as the primary tool for foundational research. Used to synthesize complex topics like the TCP lifecycle, socket networking primitives, and the `struct pollfd` architecture.
* **Google Gemini:** Used as a conceptual verifier and gap-analysis engine. I used Gemini to test my reasoning, ask targeted follow-up questions, ensure I correctly understood the underlying mechanics (e.g., hardware interrupts, wait queues, and context switching), and to debug the root causes of our benchmark anomalies.
* **Anthropic Claude:** Used strictly as a pair-programming assistant to generate baseline architectures and adapt complex state-machine logic for the `poll` server to meet strict team consistency guidelines.

## 2. Thought Process & Workflow Integration
My workflow intentionally prioritized reading and architectural comprehension over trial-and-error debugging. The workflow followed a strict pipeline:

1. **Prerequisite Understanding (NotebookLM):** Breaking down the core primitives (sockets, ports, threads, system calls) and understanding the synchronous notification paradigm of `select` and `poll`.
2. **Conceptual Verification & Deep Dives (Gemini):** Challenging my initial understanding by diving into kernel-level specifics: Rx/Tx buffers, hardware interrupts, wait queues, and the necessity of `O_NONBLOCK`.
3. **Implementation & Architectural Alignment (Claude + NotebookLM):** Generating the functional single-threaded `poll` server, then working with the AI to adapt the architecture to match our team's `epoll` baseline—specifically handling partial reads/writes, ensuring correct `EAGAIN` behavior, and optimizing array compaction.
4. **Empirical Gap Analysis (Gemini):** Employed a strict 4-step "Anti-Hallucination Workflow" to analyze benchmark data. This consisted of establishing theoretical OS baselines, ingesting the C source code and empirical data, conducting an anomaly/discrepancy detection, and executing a root-cause audit to trace performance deviations back to specific implementation flaws.

## 3. Step-by-Step Details
Here is how AI contributed to my specific learning and development stages:

### Phase 1: Foundational Networking Primitives
* **Task:** Understanding the standard TCP lifecycle.
* **AI Contribution:** Used NotebookLM to break down the exact sequence of system calls (`socket`, `bind`, `listen`, `accept`, `recv`, `send`, `close`) and how a packet traverses the network stack.

### Phase 2: Legacy Multiplexing Mechanics
* **Task:** Understanding how `select` and `poll` handle multiple connections.
* **AI Contribution:** Used NotebookLM to understand the technical implementation of the `poll` system call and the `struct pollfd` architecture. Used Gemini to conceptually compare this against the limitations of `select` (e.g., the 1024 file descriptor limit) without directly implementing the `select` server myself.

### Phase 3: Kernel Mechanics & System Bottlenecks
* **Task:** Understanding blocking I/O, context switching, and O(N) degradation.
* **AI Contribution:** Used Gemini to dive deep into what it means when a thread is "blocked." Learned how hardware interrupts wake threads from wait queues, the difference between process and thread context switches, and specifically why `select` and `poll` suffer from O(N) scaling bottlenecks under high concurrency (the C10K problem).

### Phase 4: Code Generation, State Machines & Architectural Parity
* **Task:** Implementing the single-threaded `poll` server while ensuring strict architectural parity with the team's shared codebase and `epoll` implementation.
* **AI Contribution:** Claude generated the initial C/C++ architecture for `poll`. I then used the AI to help me adapt this code to our team's strict requirements: integrating our shared `network_utils.h`, implementing a robust state machine for partial reads/writes (handling `EAGAIN`/`EWOULDBLOCK`), and designing an O(1) array compaction trick (swap-with-last) to avoid costly nested loops when clients disconnect. This phase ensured I fully grasped *why* `poll` scales poorly (the mandatory O(N) user-to-kernel memory copy) to defend in the viva.

### Phase 5: Benchmarking Data Analysis & Root Cause Debugging
* **Task:** Analyzing benchmarking data for loads up to 10,000 concurrent connections to compare the theoretical performance of the architectures against empirical reality[cite: 1].
* **AI Contribution:** Used Gemini to overlay empirical data points (throughput, latency, context switches) over the theoretical baseline. The AI successfully diagnosed that the unexpected 2.5-second tail latency spike in the `epoll` implementation was caused by an application-layer bottleneck: a synchronous `printf` head-of-line block. It also diagnosed the `select` "ghost load" (a continuous `accept`/`close` spin caused by the `FD_SETSIZE` limitation), revealed hidden overhead caused by `fcntl` syscalls during socket setup, and generated the final analysis presentation.

## 4. Key Prompts Used
Below is a curated log of the primary prompts used to drive the research and understanding phases of my contribution.

| Phase | Tool | Prompt Summary / Exact Phrasing | Purpose |
| :--- | :--- | :--- | :--- |
| **Context Setting** | NotebookLM | *"I am doing the following project and am new to all its concepts, define the terms used in it then explain the project... [Inserted full project description]"* | Established the baseline understanding of the project's end goals and O(N) vs O(1) scaling bottlenecks. |
| **Primitives** | NotebookLM | *"How would you define the following: port, socket, syscalls read() and write(), thread, bind(), listen(), accept(), close()"* | Mapped out the fundamental TCP socket lifecycle. |
| **Multiplexing** | NotebookLM | *"I wanna understand poll in depth. Generate notes covering every technical and conceptual aspect of it. Summarise the pdf from understanding struct pollfd architecture which is basically the code."* | Understood the technical implementation and specific mechanics of the `poll` system call. |
| **Packet Traversal** | Gemini | *"Bridge how the whole echo server works, and how would a normal req and response connection with TCP look like sequentially how a packet would traverse."* | Connected the API-level code to the actual network stack behavior. |
| **Kernel Deep Dive** | Gemini | *"In the context of single thread architecture, you mention the major bottleneck is that a thread could get blocked... elaborate what exactly do we mean by blocked, is it network I/O, or memory I/O? What exactly does the context switch for threads look like?"* | Clarified the difference between user-space blocking and kernel-space I/O interrupts. |
| **Implementation & Parity** | Claude | *"I am building the server_poll.c component. I need the code to perfectly match my team's architecture..."* | Guided the implementation of the `poll` server to match shared team utilities, enforce non-blocking I/O state machines, and handle partial writes correctly. |
| **Analysis Workflow Setup** | Gemini | *"we want to do analysis of what should happen then well compare with what we see then try to find out why if some desscrepensy happens... how should i manage workflow so gemini pro extended does not halucinates"* | Established a multi-phase analytical workflow ensuring the AI based its analysis on standard OS theories before ingesting our empirical results |
| **Anomaly & Gap Analysis** | Gemini | *"tell me for each of of the server what we expect to see in the graphs based on the baseline understanding and conceptual understanding and then tell what we actually see"* | Mapped our empirical benchmark data directly to the theoretical architecture, highlighting precise deviations |
| **Root Cause Audit** | Gemini | *"could you provide explaination for why each might have happened... yes do address this about the printf, also anything that we are missing out"* | Diagnosed precise implementation flaws causing benchmark anomalies, including the `printf` terminal block, the `fcntl` syscall setup tax, and `memmove` CPU penalties |

# chat links:
** 1.chatgpt- https://chatgpt.com/share/6aa690d8-3a1c-83e8-818a-b680e6cf7bc0 , https://chatgpt.com/share/6aa69132-dfe0-83e8-a76c-57f83314d9c4 , https://chatgpt.com/share/6aa68b16-c0ac-83ee-b577-5c9b2803bf25
** 2.gemini: https://share.gemini.google/QAJpKvo0lSNi , https://share.gemini.google/UmtXjsjx5Ith , 
https://share.gemini.google/EqfAD94UciEO , https://share.gemini.google/37n7Ccavyftc , 
** 3. notebookllm: https://notebook.google.com/notebook/6e02f3ef-7618-4d58-b6b2-6bf907fc4832 ( I am unable to give access, google's policy deny sharing. I have physically shown the TA before our presentation physically the chats mentioned above. 
** 4. claude: https://claude.ai/share/e1075168-e9c4-4c79-a493-aafe855f56d9 , https://claude.ai/share/6362e7fe-73d5-4624-8041-a59f0f3b53c8
** 5.the following claude session was of after the submission date- not of its content has been used in the submitted work it is purely used to try to understand what the problem was with our iouring implementation: https://claude.ai/share/026fe69a-ed99-4c21-858d-ea412377d740 , I also found following content which i explore on 12th and 13th of September 2026: https://www.smartinfralog.com/posts/post-1782652649 , https://www.alibabacloud.com/blog/io-uring-vs--epoll-which-is-better-in-network-programming_599544 , https://kernel-internals.org/io-uring/io-uring-vs-epoll/
