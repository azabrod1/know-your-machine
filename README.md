# know-your-machine

A [Claude Code](https://claude.com/claude-code) skill that turns your computer
into a physics experiment: it measures your CPU, memory hierarchy, storage, and
GPU **from first principles** — using nothing but timed loops, no spec sheets —
and then explains what every number means.

You get two HTML files back:

1. **`textbook.html`** — a chaptered, textbook-style read *for your specific
   machine*, charts woven into the story: what a clock cycle is, why one core runs
   ~6 instructions at once, your actual cache hierarchy drawn by latency cliffs,
   the branch predictor, SIMD, the TLB, the disk, and (on a Mac) the GPU.
2. **`dashboard.html`** — the same results as a dense one-page data dashboard you
   can scan, open offline, and share.

(The textbook is also written to `textbook.md` if you want plain markdown.)

## What it measures

| | question it answers |
|---|---|
| clock speed | how fast does one cycle tick? (timed dependency chain) |
| superscalar width | how many instructions does one core run at once? |
| core scaling | how does throughput grow across all cores — and where does it stop? |
| cache hierarchy | L1 / L2 / … / DRAM latency, drawn by a pointer chase |
| access pattern | why sequential is ~100× faster than random over the same data |
| branch prediction | the sorted-vs-random-array effect, and how prediction works |
| SIMD | one instruction doing 4–16 numbers at once; GFLOP/s |
| TLB | the hidden address-translation cache and its page-walk cost |
| disk | sequential vs random SSD latency; syscall amortization |
| GPU | TFLOP/s, occupancy, bandwidth (Apple/Metal; portable in principle) |

## The one rule

> Separate what you **measure** from what you **assume.** The only things truly
> measured are elapsed time and instruction counts. "GHz" and "cycles" are those
> divided, resting on one stated assumption — and the report always says so.

## Requirements

- **[Claude Code](https://claude.com/claude-code)** (the skill runs there).
- **A C compiler** — `cc`/`clang` (preinstalled on macOS) or `gcc`
  (`build-essential` on Linux). On a Mac, the Xcode command-line tools also enable
  the Metal GPU test.
- **A top-tier, high-reasoning agent — strongly recommended.** This skill doesn't
  just run a script: it asks the agent to *port* low-level benchmark code (inline
  assembly, system calls, cache/page detection) to your specific CPU, reason about
  whether each measurement is still valid, and then write a genuine explanatory
  textbook. That's demanding. Use a frontier model at high reasoning effort — e.g.
  **Claude Opus (high effort)** or a top-tier GPT reasoning model. A small or fast
  model will produce shakier ports and thinner explanations.

## What you'll get

Two self-contained HTML files for *your* machine — no dependencies, open offline,
safe to email or share:
- **`textbook.html`** — a 12-chapter illustrated read on how your chip works,
  charts woven into each chapter.
- **`dashboard.html`** — a one-page data readout of every measured number.

Plus the ported benchmark source, so you can re-run it any time.

## Install

Clone into your Claude Code skills directory:

```sh
git clone https://github.com/azabrod1/know-your-machine.git \
  ~/.claude/skills/know-your-machine
```

Then just ask Claude:

> *measure my machine*

Claude detects your hardware, adapts the benchmark to it, runs it (~30–45 s), and
writes your textbook and dashboard, then opens them.

## Works with any coding agent (Codex, etc.)

The instructions in `SKILL.md` are agent-agnostic — they're just "compile this,
run it, write these files." Any capable agent can run them:

- **Claude Code** — clone into `~/.claude/skills/know-your-machine` (above); it's
  auto-discovered. Say *measure my machine*.
- **OpenAI Codex** — clone the repo anywhere, run `codex` **inside that folder**
  (Codex auto-reads the repo's [`AGENTS.md`](AGENTS.md), which points it at
  `SKILL.md`), then say *measure my machine*.
- **Cursor / Gemini CLI / anything else** — clone it, open your agent in the
  folder, and tell it to *follow SKILL.md to measure this machine*.

Whatever the tool, **use a high-reasoning model** — porting low-level benchmark
code and explaining it well is demanding, and a small/fast model will produce
shakier ports and thinner writeups.

## Portability

The code in `example/` is a **worked reference for Apple Silicon (arm64 + macOS)**.
The skill instructs the agent to **port it to whatever machine it's running on** —
x86, Linux, Windows — keeping the *method* identical even where the inline
assembly, system calls, and cache-info APIs differ. See `SKILL.md` for how, and
`example/textbook.md` for the gold-standard write-up the agent emulates.

---

*Built by measuring an Apple M5 Max one question at a time. Every number earned
with a stopwatch.*
