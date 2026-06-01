# know-your-machine — agent instructions

This repository is a portable **"benchmark your computer and explain it"** skill.
It works with any capable coding agent (OpenAI Codex, Claude Code, Cursor, Gemini
CLI, …); this file is the entry point for agents that auto-read `AGENTS.md`.

When the user asks to **measure / benchmark this machine** (or "run
know-your-machine"):

➡️ **Follow the complete instructions in [`SKILL.md`](SKILL.md).** It is the single
source of truth. In short:

1. Detect this machine (OS, CPU, cores, RAM, cache/page sizes, GPU).
2. **Port `example/benchmark.c` to this machine.** This is the hard part and a
   real engineering task — read **SKILL.md §2** carefully and *think*, don't
   translate mechanically. Verify the measurement techniques still hold here, and
   sanity-check every number against what's physically plausible.
3. Compile (`-O2`) and run it; capture the JSON it prints.
4. (Optional) run the GPU test if one is available / portable.
5. Write the user a **`textbook.html`** (the narrative read, charts inline) and a
   **`dashboard.html`** (the data readout), modeled on the examples in `example/`.
   Teach, don't just state facts; ground each concept before measuring it.
6. Tell the user where the files are and open them.

**Use a high-reasoning model.** The porting and the explanations are demanding;
a small/fast model will produce shaky results.
