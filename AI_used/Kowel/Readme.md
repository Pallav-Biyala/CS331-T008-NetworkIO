# AI Usage Documentation

This document outlines how artificial intelligence tools were integrated into my specific portion of the team project. 

**Team Division of Labor Context:**
* **Guransh:** Implementation of the `select` and single-threaded TCP echo servers and presentation slides.
* **My Role:** Core conceptual research, kernel-level mechanics, and the implementation of the `poll` single-threaded TCP echo server.
* **Sai Krishna:** Implementation of the modern asynchronous engine `io_uring`.
* **Hemcharan:** Load generation (tcpkali/wrk), benchmarking, profiling, and final analysis.
* **Pallav:** Implementation of the modern asynchronous engine `epoll`.

For my deliverables, my primary use of AI was as a **conceptual tutor and architectural sounding board** to deeply understand Linux network stack mechanics, legacy multiplexing, and kernel/user-space transitions before implementing the code.

## 1. Tools Used
I utilized a multi-LLM approach to separate foundational learning, conceptual verification, and code generation:
* **Google NotebookLM:** Served as the primary tool for foundational research. Used to synthesize complex topics like the TCP lifecycle, socket networking primitives, and the `struct pollfd` architecture.
* **Google Gemini:** Used as a conceptual verifier. I used Gemini to test my reasoning, ask targeted follow-up questions, and ensure I correctly understood the underlying mechanics (e.g., hardware interrupts, wait queues, and context switching).
* **Anthropic Claude:** Used strictly for generating the baseline boilerplate C/C++ code for the single-threaded `poll` server, which I then studied, adapted, and documented.

## 2. Thought Process & Workflow Integration
My workflow intentionally prioritized reading and architectural comprehension over trial-and-error debugging. The workflow followed a strict three-step pipeline:

1. **Prerequisite Understanding (NotebookLM):** Breaking down the core primitives (sockets, ports, threads, system calls) and understanding the synchronous notification paradigm of `select` and `poll`.
2. **Conceptual Verification & Deep Dives (Gemini):** Challenging my initial understanding by diving into kernel-level specifics: Rx/Tx buffers, hardware interrupts, wait queues, and the necessity of `O_NONBLOCK`.
3. **Implementation Study (Claude + NotebookLM):** Generating a functional single-threaded `poll` server via Claude, and then feeding that code back into NotebookLM to map the theoretical concepts (like `POLLIN`/`POLLOUT` events) directly to the implementation.

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

### Phase 4: Code Generation & Analysis
* **Task:** Implementing the baseline single-threaded `poll` server.
* **AI Contribution:** Claude generated the initial C/C++ architecture for `poll`. I analyzed this code to understand message framing (byte streams) and how connection persistence is managed at the application level without multithreading.

## 4. Key Prompts Used
Below is a curated log of the primary prompts used to drive the research and understanding phases of my contribution.

| Phase | Tool | Prompt Summary / Exact Phrasing | Purpose |
| :--- | :--- | :--- | :--- |
| **Context Setting** | NotebookLM | *"I am doing the following project and am new to all its concepts, define the terms used in it then explain the project... [Inserted full project description]"* | Established the baseline understanding of the project's end goals and O(N) vs O(1) scaling bottlenecks. |
| **Primitives** | NotebookLM | *"How would you define the following: port, socket, syscalls read() and write(), thread, bind(), listen(), accept(), close()"* | Mapped out the fundamental TCP socket lifecycle. |
| **Multiplexing** | NotebookLM | *"I wanna understand poll in depth. Generate notes covering every technical and conceptual aspect of it. Summarise the pdf from understanding struct pollfd architecture which is basically the code."* | Understood the technical implementation and specific mechanics of the `poll` system call. |
| **Packet Traversal** | Gemini | *"Bridge how the whole echo server works, and how would a normal req and response connection with TCP look like sequentially how a packet would traverse."* | Connected the API-level code to the actual network stack behavior. |
| **Architecture** | Gemini | *"Explain single thread architecture and multi thread architecture, in a single process server. Why did we use single thread architecture as opposed to multi thread architecture in the project?"* | Justified the project's architectural constraints for the `select` and `poll` mechanisms. |
| **Historical Context** | Gemini | *"Walk me through how servers work, the history of how it was and how it is going. What is the C10K problem in server architecture, and how did tools like Nginx solve it?"* | Provided historical context for the bottlenecks I observed in `select` and `poll`. |
| **Kernel Deep Dive** | Gemini | *"In the context of single thread architecture, you mention the major bottleneck is that a thread could get blocked... elaborate what exactly do we mean by blocked, is it network I/O, or memory I/O? What exactly does the context switch for threads look like?"* | Clarified the difference between user-space blocking and kernel-space I/O interrupts. |
