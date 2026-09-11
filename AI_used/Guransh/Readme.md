# AI Usage Disclosure

**Name:** Guransh Kaur Saran
**Contribution to project:** Implemented the `select()`-based server, made the presentation (PPT), and contributed to the report section for the select server.

---

## Tools Used

- **Claude** – mainly used for debugging the `select()` server code, understanding syscall behaviour, and getting help structuring the PPT content.
- **Gemini** – used for quick doubts, generating some explanations for slides, and double checking a few socket programming concepts.

---

## Prompts Used

Below are (roughly) the prompts I gave, kept in the same wording I actually typed:

1. "hey can u explain what is select(), epoll(), poll()? and how select() works in socket programming, like whats fd_set and why do we need FD_ZERO FD_SET etc"
2. "im writing a server in C using select(), act like a teacher and help me write this step by step"
3. "whats the diff between select and poll and epoll in simple words, need this phrased for a report"
4. "how do i handle the case when select() returns but its the listening socket thats ready vs a client socket, kinda confused on the loop logic"
5. "can u give me a rough outline for ppt slides on select() based server, like what should go in the slide"
6. "make the slides sound more professional and interesting to read”
7. "why is my FD_ISSET check not working properly"
8. "explain select() server limitations for my report, like why its not scalable for many clients"
9. "can u just check if this explanation of select() is technically correct"
10. "need a simple diagram (like text diragram) for ppt showing select() monitoring multiple fds at once"

---

## Thought Process / How AI Was Integrated into My Workflow

I used AI to write the select server code for me from scratch, but I asked it to build up the code step by step and teach me everything. Whenever I got stuck on a specific bug (like the server not accepting a second client, or `max_fd` not updating properly), I'd paste the relevant snippet into Claude and ask it to point out the issue rather than rewrite the whole file. I'd then go back and fix it myself in the code.

For conceptual doubt, I used both Claude and Gemini almost like a doubt-clearing session, asking follow-up questions until it made sense to me, since I needed to explain some of this in the report too.

For the PPT, I used AI more for structuring/wording than content generation, I already knew what I wanted to present, but I used Claude to help me turn my rough explanations into slide-friendly bullet points. Gemini was used as a quick "sanity check" to verify a couple of technical statements before putting them on slides, since I didn't want to present something factually wrong.

Overall, AI was used as a teacher + debugging aid + concept-clarifier + slide-phrasing helper. The core logic of the select server, the actual report writing, and the final slide content/design were done by me.

---

## Step-by-Step Details of AI Contribution

| Stage | Where AI helped | How |
|---|---|---|
| Initial select() server coding | Understanding syscall | Asked Claude to explain `select()`, `fd_set`, `FD_ZERO`/`FD_SET`/`FD_ISSET`/`FD_CLR` before I started writing the accept/read loop |
| Debugging (listening vs client socket | Bug fixing | Pasted my code into Claude, it pointed out I wasn't handling the listening socket vs client socket check properly inside the loop |
| Debugging (max_fd issue) | Bug fixing | Asked Claude why `FD_ISSET` checks were failing, turned out I wasn't updating `max_fd` after adding a new client fd, fixed this myself after the explanation |
| Report writing (select server section) | Concept clarity only | Asked Claude/Gemini to explain select vs poll vs epoll and why select has a scalability limit (fd_set size limit, O(n) scanning), then used it to write the actual report paragraphs using that understanding |
| PPT structuring | Outline suggestion | Asked Claude for a rough slide-by-slide outline for presenting a select() server (intro → syscall → server loop → limitations → demo) |
| PPT wording | Bullet-point phrasing | Gave Claude my own explanations and asked it to condense them into short slide-friendly bullets |
| PPT fact-check | Verification | Asked Gemini to check if a couple of technical statements about select() were correct before finalizing slides |
| Diagram idea | Visual planning | Asked Claude for a simple way to describe/draw select() monitoring multiple fds, used this idea to make one of the diagram slides myself |

