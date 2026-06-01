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

## Install

Clone into your Claude Code skills directory:

```sh
git clone https://github.com/<you>/know-your-machine.git \
  ~/.claude/skills/know-your-machine
```

Then just ask Claude:

> *measure my machine*

Claude detects your hardware, adapts the benchmark to it, runs it, and writes
your report and dashboard.

## Portability

The code in `example/` is a **worked reference for Apple Silicon (arm64 + macOS)**.
The skill instructs the agent to **port it to whatever machine it's running on** —
x86, Linux, Windows — keeping the *method* identical even where the inline
assembly, system calls, and cache-info APIs differ. See `SKILL.md` for how, and
`example/textbook.md` for the gold-standard write-up the agent emulates.

---

*Built by measuring an Apple M5 Max one question at a time. Every number earned
with a stopwatch.*
