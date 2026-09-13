# AI Usage Disclosure

**Name:** Guransh Kaur Saran

**Contribution to project:** Implemented the `select()`-based server, made the presentation (PPT), and contributed to the report section for the select server.

---

## Tools Used

- **Claude** – mainly used for debugging the `select()` server code, understanding syscall behaviour, and getting help structuring the PPT content.
- **Gemini** – used for quick doubts, generating some slides.

---

## Prompts Used

### Claude Chats
- [Chat 1](claude%20chat%201.md)
- [Chat 2](claude%20chat%202.md)

### Gemini Chats
- [Chat 1](gemini%20chat%201.pdf)
- [Chat 2](gemini%20chat%202.pdf)

---

## Thought Process / How AI Was Integrated into My Workflow

I used AI to write the select server code for me from scratch, but I asked it to build up the code step by step and teach me everything. Whenever I got stuck on a specific bug (like the server not accepting a second client, or `max_fd` not updating properly), I'd paste the relevant snippet into Claude and ask it to point out the issue rather than rewrite the whole file. I'd then go back and fix it myself in the code.

For conceptual doubt, I used both Claude and Gemini almost like a doubt-clearing session, asking follow-up questions until it made sense to me, since I needed to explain some of this in the report too.

For the PPT, I used AI more for restructuring than content generation, I already knew what I wanted to present, but I used Claude to help me turn my explanations into slides. Gemini was used as a quick helper.

Overall, AI was used as a teacher + debugging aid + concept-clarifier + slide-helper. 

---

## Step-by-Step Details of AI Contribution

| Stage | Tool | Where AI helped | How |
|---|---|---|---|
| Understanding `select()` | Claude | Concept clarity | Asked Claude to explain `select()`, `fd_set`, and `FD_ZERO`/`FD_SET`/`FD_ISSET`/`FD_CLR`, plus what "blocking" actually means, before writing the accept/read loop |
| Writing the select-loop code | Claude | Code scaffolding | Built the server skeleton, client-fd array, the `select()` loop, and the accept/read/echo logic piece-by-piece with Claude |
| Debugging (missing header) | Claude | Bug fixing | Compiler said `accept()` was implicitly declared; Claude spotted that `sys/socket.h` wasn't included in `select_server.c` |
| Debugging (`perror` misuse) | Claude | Bug fixing | Compiler flagged "too many arguments to `perror`"; Claude explained `perror()` isn't `printf`-style and fixed the line to use `printf` instead |
| Debugging (non-blocking client sockets, FD_SETSIZE, EAGAIN) | Claude | Bug fixing / code review | After teammates ran the code past Gemini as well, pasted the consolidated feedback to Claude, which turned it into concrete fixes: making client sockets non-blocking after `accept()`, adding an `FD_SETSIZE` guard, and handling `EAGAIN`/`EWOULDBLOCK` on `read()` instead of treating it as a disconnect |
| Report writing (select() limitations) | Claude + Gemini | Concept clarity | Asked Claude to explain the `FD_SETSIZE = 1024` cap and O(N) scanning for the report's limitations section; separately asked Gemini for a select vs poll vs epoll vs io_uring restaurant analogy to frame the same scaling argument |
| Graph fact-check | Gemini | Verification | Asked Gemini why the `server_select` context-switch line disappears/overlaps in the benchmark graph, to get an accurate explanation before writing the caption |
| Slide content (restaurant analogy) | Gemini | Slide-content generation | Had Gemini shorten the select/poll/epoll/io_uring restaurant analogy into slide-ready bullets, then fold it into a single existing slide layout |
| Slide deck build | Claude | File generation | Gave Claude the finished `presentation_slides.md` plus the 4 benchmark graph images and had it generate the actual 6-slide `.pptx` (fixed slide count according to updated structure) |
