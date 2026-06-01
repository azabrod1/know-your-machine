---
name: know-your-machine
description: >-
  Measure THIS computer's CPU, memory hierarchy, storage, and GPU from first
  principles — using nothing but timed loops — then write a personalized
  textbook-style report and a one-page HTML dashboard of the results. Use when
  the user wants to benchmark their machine, understand how fast their computer
  really is, see their cache/memory hierarchy, or generate a hardware
  performance write-up. The included code is an Apple-Silicon reference EXAMPLE;
  the agent ports it to the user's actual machine.
---

# Know Your Machine

Turn the user's computer into a physics experiment: measure its clock speed,
superscalar width, core scaling, cache hierarchy, branch predictor, SIMD width,
TLB, disk, and GPU — all by timing small programs — then explain what every
number means in a guided, textbook-style write-up plus a shareable one-page
HTML visualization.

## What ships in this skill

```
example/
  benchmark.c   reference C program (Apple Silicon): all CPU/memory/disk tests,
                emits JSON on stdout, progress on stderr
  gpubench.m    reference Metal program: GPU TFLOP/s, occupancy, bandwidth
  textbook.md     the EXAMPLE textbook in markdown (the gold-standard voice/structure)
  textbook.html   the same textbook as HTML, charts woven into the chapters
  dashboard.html  a complete EXAMPLE one-page data dashboard (the scan-it view)
```

Two HTML deliverables, two jobs: **`textbook.html`** is the deep read (the
chaptered narrative with its graphics inline); **`dashboard.html`** is the
glance (dense numbers + charts, one insight line each). Generate both, and
**cross-link them** — each page carries a small `[ Textbook | Dashboard ]` toggle
in its header (current view highlighted, the other a *relative* `href`, since
both files land in the same folder; this keeps them self-contained and offline).

**The example is written for Apple Silicon (arm64 + macOS).** Your job is to
*port it to the machine you're actually running on*, run it, and produce the
equivalent report + dashboard for that machine. The example shows the target
quality; it is not expected to run unchanged on Windows or x86 Linux.

## The one rule (carry it into the report)

> Separate what you **measure** from what you **assume.** The only things truly
> measured are *elapsed time* and *instruction counts you control*. "GHz,"
> "cycles," "IPC" are those divided, resting on one stated assumption (a
> dependent integer add = 1 cycle). When an assumption can't be verified, say so.

This honesty is the soul of the report. Never present an inferred number as if
it were directly measured. Label best-of-N (ceiling) vs single-run (typical).

## Procedure

### 1. Detect the machine
Find OS, architecture, CPU model, core counts (performance vs efficiency if
applicable), RAM, cache sizes, page size, and whether a usable GPU exists.
- macOS: `sysctl -a | grep -E 'machdep.cpu|hw.(perflevel|l1|l2|memsize|cacheline)'`
- Linux: `/proc/cpuinfo`, `lscpu`, `getconf PAGE_SIZE`, `/sys/devices/system/cpu/.../cache`
- Windows: `wmic`/PowerShell `Get-CimInstance`, `Get-ComputerInfo`

### 2. Port the benchmark to this machine
Read `example/benchmark.c`. Everything platform-specific is tagged `[[PORT]]`.
Adapt, keeping the *method* identical even when the code changes:
- **Inline asm** is arm64 (`add`, `cmp/b.lo`, `csel`, NEON `.4s`, `fmla`). On
  x86-64 use the equivalent mnemonics/intrinsics (SSE/AVX), or a portable-C
  fallback with `volatile`/compiler barriers when precise control isn't critical.
- **Forced branch** (branch-prediction test): must stay a real, compiler-proof
  branch. Inline asm is cleanest; do NOT rely on `-O0` (it de-optimizes
  everything and ruins the numbers — see `textbook.md` Chapter 7).
- **CPU/cache info**: `sysctl` → `/proc` or registry.
- **Uncached disk I/O**: `F_NOCACHE` (macOS) → `O_DIRECT` (Linux) → `FILE_FLAG_NO_BUFFERING` (Windows). Always: check free space, write a modest temp file ONCE, `unlink` it immediately (or delete in a `finally`), prefer reads.
- **Page size**: don't hardcode 16 KB; read it (x86 is 4 KB) for the TLB test.

Compile with optimization on (`-O2`). Run; capture the JSON from stdout.

### 3. (Optional) GPU
If an Apple GPU is present, build/run `example/gpubench.m`
(`clang -fobjc-arc -O2 -framework Metal -framework Foundation`). On other GPUs,
either port to the platform's compute API (CUDA/OpenCL/Vulkan) or note it as not
measured — be explicit about which.

### 4. Be responsible
These are pure userspace loops and reads — they can't damage hardware (thermal
throttling protects it), but: keep the disk temp file small and always delete
it; don't fill the disk; warn before any sustained all-core run if the user is
on battery or doing other work.

### 5. Write the textbook as HTML (model on `example/textbook.html`)
Produce a chaptered, narrative textbook personalized to THIS machine's numbers,
as a self-contained `textbook.html` (inline CSS + inline SVG, opens offline) with
the charts woven into the chapters — same prose voice as `textbook.md`, but HTML
so the graphics live inside the story. Match the example's voice: each chapter
asks one question, shows the measured result (as a small chart or result block),
explains the mechanism in plain language, states the honest caveats, and sets up
the next. **Teach — don't just state facts.** Assume the reader has never heard
of the concept: build from something familiar, explain how the mechanism actually
works (a concrete picture, tiny diagram, or analogy where it helps), and say what
the numbers *mean* and whether they're high or low. A paragraph that only asserts
("a SIMD register holds four, so it does four at once") has failed; show *why* and
*what it buys*. Aim for real understanding, not a caption. Carry the two recurring
themes the data keeps proving:
- **Independence (ILP)** — dependent vs independent work; the speed of overlap.
- **Locality** — is the data reachable cheaply; the cache/prefetcher story.
End with the full hierarchy ladder (registers → … → disk) and, if measured, the
CPU-vs-GPU "two philosophies of hiding latency" finale. Interpret the user's
*actual* numbers (don't copy the example's); compare to typical hardware so they
know if a result is high/low. (Optionally also emit a plain `textbook.md`.)

### 6. Generate the dashboard (model on `example/dashboard.html`)
A single self-contained `.html` file (inline CSS + inline SVG, no external
dependencies, opens offline). **Data leads; numbers and charts up front** — but
cap each panel with **one sharp insight line** (the "so what"), never paragraphs.
Pack in THIS machine's measured values: headline stat tiles, the memory-hierarchy
table with the "1 cycle = 1 second" scale, inline-SVG line charts for every sweep
we have (cache-latency-vs-size, core scaling, bandwidth saturation, disk
block-size, GPU occupancy) — every chart with **labeled axes**: y-tick values, an
x-label at every data point, and value callouts on the key points (a chart with
only bare endpoint labels hides the data and is not acceptable) — and compact
key/value cards for the scalar results
(superscalar, SIMD, branch, TLB, access pattern, disk, GPU, CPU-vs-GPU), each with
its one-liner. The long-form narrative lives in `textbook.html`; the dashboard is
the data plus a single takeaway per panel.

### 7. Walk the user through what they got and how to open it
Don't assume they know where the files are or what they are — spell it out, with
full paths:
- **`textbook.html`** — *the read.* Open in a browser to learn how their machine
  works, chapter by chapter, with the charts inline.
- **`dashboard.html`** — *the scan.* A one-page visual of all the numbers.
- **the benchmark source** — keep it; recompile and re-run anytime to compare.

Both HTML files are fully self-contained (no internet, no dependencies): they can
be double-clicked from Finder / File Explorer, opened offline, or emailed and
shared as-is. Then actually open them for the user:
- macOS: `open textbook.html dashboard.html`
- Linux: `xdg-open textbook.html` (and `dashboard.html`)
- Windows: `start textbook.html` (and `dashboard.html`)

## Deliverables
1. The ported benchmark source (so the user can re-run it).
2. `textbook.html` — their machine's textbook (narrative + graphics inline).
3. `dashboard.html` — their machine's one-page data dashboard.
