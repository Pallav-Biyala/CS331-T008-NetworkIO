## AI Usage Disclosure

### Tools

The following AI tools were used during this project:

- Claude in Antigravity
- GitHub Copilot in Visual Studio Code

### Prompts

During the ideation and learning stage, I discussed questions such as:

- What are `select`, `poll`, `epoll`, and `io_uring`?
- Why is `io_uring` needed, and how does it work internally?
- How do these approaches differ in scalability, latency, system calls, and CPU usage?
- What are the advantages and disadvantages of `io_uring`, including `SQPOLL`?
- How should the different server implementations and benchmarks be designed?

During implementation, I explained to the AI how I wanted the server, networking functions, benchmarking scripts, and analysis tools to be structured. I also gave prompts asking the AI to write or improve specific parts of the code, while keeping the project requirements and existing design in mind.

### Thought Process

AI was first used as a discussion and learning aid. I asked questions about network I/O models and `io_uring` to clarify the concepts before writing the implementation. This helped me understand what each method does, why `io_uring` can reduce overhead, and the practical trade-offs and limitations of each approach.

After developing the conceptual understanding, I used Claude in Antigravity and GitHub Copilot in VS Code during coding. I described the required behavior and implementation approach, and the AI generated initial code and suggested changes. I then went through the generated code, read and understood how it worked, checked it against the project requirements, and tested or reviewed the relevant results.

The AI was used as an assistant, not as a replacement for understanding or verification. In some parts of the project, I manually modified small sections of the generated code to correct details, match the project structure, or implement the behavior I needed.

### Step-by-Step Details

1. **Ideation:** Discussed the project idea with AI and explored what `io_uring` is, why it is useful, and how it compares with `select`, `poll`, and `epoll`.
2. **Concept clarification:** Asked AI to explain the internal operation, strengths, weaknesses, scalability, latency behavior, system-call overhead, and `SQPOLL` mode of `io_uring`.
3. **Design planning:** Used the discussions to decide which server implementations, networking operations, and performance parameters should be compared.
4. **Code generation:** Explained the required functionality and structure to Claude in Antigravity and GitHub Copilot in VS Code. They generated initial implementations and code suggestions.
5. **Code review and understanding:** Read through the generated code and understood the control flow, socket handling, event loops, error handling, and `io_uring` operations.
6. **Manual refinement:** Modified small parts of the code where necessary to fix details, adapt the implementation to the repository, or match the intended behavior.
7. **Benchmarking:** Used AI assistance to discuss benchmark design, including throughput, latency, system calls, context switches, CPU usage, and memory measurements.
8. **Results analysis:** Used AI to help inspect benchmark output, identify parsing issues, compare raw logs with generated graphs, and explain limitations in the measurements.
9. **Final verification:** Reviewed the generated code and results and retained responsibility for understanding the implementation and deciding which changes were appropriate.

