# How a CPU Really Works — Measured From Scratch

A short course in what makes a processor fast or slow, taught entirely by
*measuring one*. No spec sheets, no system APIs that hand you the answer — just
small C programs (plus one Metal program for the GPU), a stopwatch, and a few
honest tricks. Each chapter asks one question, builds the smallest experiment
that answers it, and the answer sets up the next question.

Everything here was run on an **Apple M5 Max** — 6 performance cores, 12
efficiency cores, ~4.5 GHz. Your machine will print different numbers; the
*shape* of every result will be the same.

```sh
# build everything
for f in cpuspeed widthtest superscalar threadscale memlatency memcompare \
         branchpredict branchperiod plaincif simd tlbtest disktest chipwide; do
  cc -O2 -pthread -o "$f" "$f.c"
done

# the GPU benchmark (Chapter 12) is Metal, built separately:
clang -fobjc-arc -O2 -framework Metal -framework Foundation -o gpubench gpubench.m
```

> **The one rule of this course: separate what you *measure* from what you
> *assume*.** We only ever truly measure two things — *elapsed time* (the OS
> clock, which we cross-check against a second hardware clock) and *instruction
> counts* (we write the instructions, so we know them exactly). Everything else
> — "GHz," "cycles," "IPC" — is those two divided, resting on one stated
> assumption. When an assumption can't be proven from inside the program, the
> text says so. That honesty is the difference between measuring a machine and
> fooling yourself.

---

## Chapter 1 — What is a clock cycle, and how fast does it tick?

A CPU is driven by a clock; one *cycle* is one tick. "4.5 GHz" means 4.5 billion
ticks a second. On Apple Silicon you can't just read that number — the OS won't
tell you, and the true hardware cycle counter is privileged. So we *infer* it,
with the trick the whole course is built on:

> Build a chain of **dependent** integer adds — `x=x+1; x=x+1; …` — where each
> add needs the previous one's result. The CPU physically cannot start the next
> until this one finishes, and an integer add finishes in exactly one cycle. So
> the chain advances at **precisely one add per cycle**, and *adds-per-second =
> cycles-per-second = clock speed.*

`cpuspeed.c` runs that chain and clocks it at **~4.57 GHz**.

**Measured vs assumed.** We measured time and counted adds; that part is solid.
The leap to "GHz" assumes one dependent add equals one cycle. Is that safe?
`superscalar.c` times the same work on *two independent clocks* — the OS
monotonic timer and the ARM architectural timer (`cntvct`) — and they agree to
**0.00%**, so the *time* is trustworthy. The cycle assumption itself we can't
prove from userspace (the real cycle counter is off-limits), but it lands
exactly on the known P-core clock, so it's corroborated. Honest label for our
output: *"billions of dependent adds per second,"* which we call GHz.

One lovely detail falls out of the cross-check: the ARM system timer ticks at a
*fixed* 1 GHz while the core runs at ~4.5 GHz. A chip has several clocks — a
steady one for telling time, a variable one for doing work. We've been measuring
the second through the first.

---

## Chapter 2 — A single core does many things at once

Here is the fact that surprises almost everyone. The mental model of "fetch an
instruction, do it, fetch the next" has been wrong for thirty years. A modern
core is **superscalar**: it has *several* execution units and fires multiple
instructions every cycle.

We can see it directly. Take that dependent chain (1 add/cycle) and compare it
to several chains that *don't* depend on each other. `superscalar.c` does
exactly 8 billion adds each way, on one core:

```
one dependent chain  : 1.78 s  ->  4.49 billion adds/sec   (1 add/cycle)
six independent chains: 0.38 s  -> 21.19 billion adds/sec   (4.7 adds/cycle)
```

Same core, same 8 billion adds, **4.7× faster** when they're independent —
because the core ran ~5 of them *simultaneously*. The dependent chain wasn't
slow because adds are slow; it was slow because each add had to *wait* for the
one before. Remove the waiting and the core's real width shows.

This is the course's first recurring theme: **independence is speed.** A
dependency chain pins you to 1-per-cycle no matter how powerful the hardware is.

---

## Chapter 3 — …but only if it can *see* the parallelism

If a core is ~6 wide, why did `cpuspeed`'s sweep flatten at only ~3.2 adds/cycle?
The answer is a great lesson in how out-of-order execution actually works — and
it was *my code's* fault, not the chip's.

A core finds independent work by looking *ahead* in the instruction stream, but
only within a finite **window** (its scheduler). In my first version each chain
was a contiguous block — 64 adds to `a`, then 64 to `b`, then 64 to `c`. The
only independent work was a full 64 instructions away, often beyond the window,
so execution units sat idle. `widthtest.c` proves it by running the *same* 8
chains two ways:

```
BLOCKED layout    : 3.21 adds/cycle   (a,a,a,…  then b,b,b,…)
INTERLEAVED layout: 5.67 adds/cycle   (a,b,c,d, a,b,c,d, …)
```

Identical work, merely reordered, and throughput nearly doubles to **~5.7
adds/cycle** — right at the core's true integer width. When independent
instructions sit *next to* each other, the core sees them instantly and keeps
its units full.

That apparent "hardware ceiling" was an illusion created by how the work was
laid out. This is precisely why compilers perform **instruction scheduling** —
shuffling independent operations together so the out-of-order engine can exploit
them. We just watched that optimization earn ~2× by hand.

---

## Chapter 4 — Now multiply by all the cores

Chapters 2–3 lived inside *one* core. This chip has 18. `threadscale.c` runs the
wide loop on 1→32 threads (time-based, so fast P-cores and slow E-cores
aggregate fairly):

```
 threads   Gadds/s   speedup
    1        21.2      1.0x
    8       151.7      7.2x
   16       287.5     13.6x
   24       318.0     15.0x   (oversubscribed — only 18 cores exist)
   32       317.8     15.0x   (identical: the plateau IS the core count)
```

It scales near-linearly to ~16 threads, then slams into a **dead-flat wall**:
24 threads and 32 threads return the *exact same* 318 Gadds/s, because there is
no 19th core to run on. Past the physical core count, extra threads just take
turns and add nothing. That flat line is the hardware making its own count
visible. Peak ≈ **318 billion adds/sec**.

Why 15× and not 18×? Two honest reasons: the 12 E-cores are genuinely slower
than the 6 P-cores, and under all-core load the whole chip shares one power
budget, so the P-cores can't hold their single-thread boost clock. 18 cores of
*hardware* deliver ~15 cores of *single-thread-boost* throughput. Expected, not
a flaw.

So far the story is all upside: pipelining, superscalar width, 18 cores. The
naive "one instruction at a time" model undersells this chip by ~70× on compute
alone. But all of that assumed the data was *right there*. The next two chapters
are about what happens when it isn't.

---

## Chapter 5 — The memory wall

Every add so far touched only registers. Real code constantly reaches into
memory, and that is where cores go to *wait*. We measure it with a **pointer
chase**: a buffer wired into one big cycle where each step is `p = next[p]`, so
each load's address depends on the previous load's result — no overlapping, no
guessing ahead. Pure, exposed latency. `memlatency.c` sweeps the buffer size:

```
  footprint   ns/access   cycles
    128 KB      0.87         4     <- lives in L1 data cache
      8 MB      9.32        41     <- L2
    256 MB     95.61       425     <- main memory (DRAM)
```

The latency climbs in *cliffs* as the working set outgrows each cache, drawing
the memory hierarchy with nothing but timing. An L1 hit costs **4 cycles**; a
DRAM access costs **~425** — over 100× worse. During one DRAM stall, a 6-wide
core could have executed ~2,500 instructions; instead it's frozen, waiting for a
single number to arrive. *This* is why real programs rarely approach the 318
G/s ceiling — they spend a huge fraction of their lives stalled on memory, and
caches, prefetchers, and out-of-order execution all exist to fight that stall.

---

## Chapter 6 — Access pattern beats working-set size

The pointer chase was the *worst* case: random and dependent. What if we keep
the buffer huge but make the accesses *predictable*? `memcompare.c` walks the
same buffers two ways — a shuffled cycle vs. a sequential one (`next[i]=i+1`) —
changing nothing but the order:

```
  footprint   RANDOM     SEQUENTIAL   speedup
   128 KB     0.88 ns     0.87 ns       1×
    64 MB    55.19 ns     0.89 ns      62×
   256 MB    96.52 ns     0.88 ns     109×

Streaming 256 MB, independent loads: 0.04 ns/access -> 99 GB/s (one core)
```

The sequential column is a **flat line.** A 256 MB sequential walk costs the
same as a 16 KB one — because the hardware **prefetcher** spots the regular
stride and pulls each cache line in *before* the core asks for it. Over the
identical 256 MB, random access is **109× slower.** Working-set size stopped
mattering the instant the access became predictable.

And dropping the dependency too (independent streaming loads) hits **99 GB/s on
a single core** — the same "independence is speed" lesson from Chapter 2, now on
loads instead of adds.

These two chapters give the two knobs that govern real performance: **locality**
(is the data reachable cheaply? up to ~100×) and **independence** (is there
other work to overlap while you wait? up to ~6–20×). Multiply them and you get
the ~2,000× canyon between best-case compute and worst-case memory that every
real program lives inside.

---

## Chapter 7 — Branches, and how a CPU "predicts" the future

There's a third way to stall a core: **control flow.** A pipelined CPU starts
work on later instructions before earlier ones finish — but at an `if`, it
doesn't yet know which way execution will go. So it **guesses**, runs ahead
speculatively, and pays a penalty (~15 cycles of pipeline flush) only if it
guessed wrong.

### The famous experiment

Sum the elements `>= 128` in a big array, sorted vs. unsorted — *identical work,
identical sequential memory, same array in L1.* `branchpredict.c`:

```
  kernel       data           cycles/elem
  branch       sorted            1.46     ┐ predictable
  branch       random            4.67     ← 3.2× slower!
  branch       alternating       1.46     ┘ predictable
  branchless   sorted            0.97     ┐ flat — data doesn't
  branchless   random            0.97     ┘ matter at all
```

The random branch is **3.2× slower** — purely from mispredictions. In sorted
data the condition is false, false, …, then true, true — trivially predictable.
In random data it's a coin flip the predictor can't beat.

### So what *is* "prediction"? (It's not AI.)

It isn't your code, and it isn't intelligence. It's a small dedicated circuit
that exploits one dumb fact: **branches in real programs are absurdly
repetitive.** "Do what this branch did last time" is right 95–99% of the time.
The classic mechanism is a **2-bit saturating counter** per branch: a tiny state
machine that leans "taken" or "not taken" and needs to be wrong *twice* to flip.
That's it — a number in a table and a +1/−1 rule, running automatically beneath
every program. It's "prediction" the way *"the bus was late yesterday, so it'll
be late today"* is prediction: pattern memory, not thought.

### Proving it's the predictor, not the compiler

The skeptical question — *"maybe the compiler just optimized the easy cases"* —
is exactly right to ask, and there are two airtight answers.

First, the compiler **can't**: `sum_branch` is one function, compiled once (you
can see its single hand-written `b.lo` in the disassembly), and we call it with
arrays built at *runtime*. It never sees the data, so it cannot specialize.

Second, we *show* it with `branchperiod.c`. Same function, and every array is
the identical 64 KB of `{50, 200}` bytes — we vary only the **period** at which
the branch-outcome pattern repeats:

```
  outcome period   cycles/elem
        1            0.96     ┐ trivially predictable
        2            0.97     ┘ alternating
        8            1.33
       64            2.13      pattern getting too long to hold
      512            2.70
     4096            2.97     ┐ exceeds the predictor's memory
   random            5.09     ┘ but true randomness is still worst
```

A single compiled function produces a smooth curve from 0.96 to 5.09 cycles that
depends *only on the runtime periodicity of the data.* A compiler cannot do
that; only a hardware predictor with finite history can. And notice it predicts
**period-2 alternating perfectly** (0.97) — a plain 2-bit counter would *thrash*
on strict alternation. So Apple's predictor is the smarter **history-based** kind
(it keys on the recent pattern of outcomes), and this curve even sketches *how
long* a pattern it can memorize (hundreds of branches before it gives up).

### Why we forced the branch by hand — and not just `-O0`

A natural shortcut: instead of writing the branch in assembly, just compile
without optimization so the compiler keeps our `if`. `plaincif.c` shows why
that's a trap. The plain-C `if`, compiled two ways:

```
-O2:  sorted 0.06 ns/elem,  random 0.05 ns/elem   -> NO difference
-O0:  sorted 0.59 ns/elem,  random 1.58 ns/elem   -> 2.66× difference
```

At `-O2` the compiler turned the `if` into branchless vector code (0 conditional
branches in the binary — it even auto-used SIMD, hence 0.05 ns/elem), so the
branch effect *vanishes entirely.* That's why studying branches needs a branch
the compiler can't delete — hence inline assembly. At `-O0` the branch survives
(2 branches, effect appears) — but everything else is de-optimized and ~10×
slower, so the numbers no longer describe the real machine. Inline assembly is
the scalpel: keep realistic `-O2` everywhere, pin *only* the one instruction we
want to study.

### The fix

Notice the `branchless` rows above are **flat** — identical for sorted and
random. Replacing the branch with a conditional-select (`csel`) removes the
data-dependence completely and runs ~5× faster than the mispredicting version.
That's both the real-world fix and the final proof the branch was the culprit.

(One honest correction: the *effective* misprediction penalty we measured is
~6 cycles, not the textbook ~15. The raw flush is deeper, but the out-of-order
engine overlaps the recovery with independent upcoming loads — the Chapter 2
machinery quietly refunding part of the cost.)

---

## Chapter 8 — SIMD: one instruction, many numbers

Every add so far handled one number. The last big lever inside a core is
**SIMD** (Single Instruction, Multiple Data): a 128-bit NEON register holds four
32-bit ints, and one `add.4s` adds all four at once. With several SIMD units, the
lanes multiply. `simd.c`, one core:

```
  scalar  int add  :  5.9 int-adds / cycle   (our familiar baseline)
  NEON    int add  : 16.6 int-adds / cycle   (2.8× — 4 lanes × several units)
  NEON    float FMA : 75.9 GFLOP/s  =  17.3 flops / cycle
```

A single core does **~73 billion integer adds per second** with SIMD — over 3×
its own scalar peak, and faster than anything one core managed earlier in the
course.

The float number teaches the recurring lesson one more time. A fused
multiply-add (`fmla`) does 8 flops per instruction, and the chip has ~4 FP units
— so the *peak* is ~32 flops/cycle. We measured ~17, only half, because the test
used just 8 independent accumulators and FMA has a ~4-cycle latency: 8 chains ÷
4 cycles ≈ 2 instructions/cycle. **It's ILP-limited, not unit-limited** — the
very same "independence is speed" wall from Chapter 2, now on floating point.
More accumulators would roughly double it toward the true peak — and in Chapter
11 we do exactly that: **16 chains hits 140 GFLOP/s = 32 flops/cycle**, which is
all 4 FP units saturated. (8 chains → 76, 16 → 140: the prediction, confirmed.)

---

## Chapter 9 — The hidden cache: address translation (the TLB)

Every memory access uses a *virtual* address that the hardware must translate to
a *physical* one before it can touch the cache or RAM. That translation is itself
cached, in a small dedicated structure called the **TLB** (Translation Lookaside
Buffer). Miss the TLB and the CPU must "walk the page tables" — several extra
memory reads to find the mapping — even if the data you want is sitting right
there in L1.

To see the TLB *by itself*, separated from the data cache, `tlbtest.c` chases the
same number of cache lines two ways — so the data-cache pressure is identical and
*only the page count differs*:

```
            DENSE (lines packed)   SPARSE (1 line per 16 KB page)
   lines       (few pages)            (one page each)        TLB cost
    1024        3.9 cyc                116.7 cyc             +113 cyc
    4096       11.6 cyc                207.7 cyc             +196 cyc
  131072       79.5 cyc                214.1 cyc             +135 cyc
```

Look at the `1024` row: it's the *same 128 KB of data*, sitting in L1 both times,
yet spreading it across 1024 pages makes it **30× slower.** The data was never
the problem — the core simply couldn't *translate the address* fast enough,
because 1024 pages overflow the TLB. A full page-walk costs ~200 cycles, as
expensive as a DRAM miss. So there are really *two* caches working on every
access: one for the data, and this hidden one for the addresses — and you can
blow either. This is also the concrete payoff of the 16 KB page size: bigger
pages mean each TLB entry covers 4× more memory, so the TLB reaches further.

---

## Chapter 10 — All the way down: the SSD

Below DRAM sits storage. `disktest.c` measures it honestly — `F_NOCACHE` forces
real device I/O (or macOS would serve it from the 128 GB RAM cache and we'd just
be re-measuring DRAM), and the temp file is `unlink`ed the instant it's created,
so it's reclaimed on exit even if the program crashes.

```
  sequential write (1 MB blocks)  :  9.58 GB/s
  random read (16 KB)             : 71.5 µs   (~315,000 cycles of stall)
```

That random-read latency is the SSD's pointer-chase, and it's **brutal: ~760×
slower than DRAM, ~80,000× slower than L1.** But the headline lesson is about
*how* you talk to it. Every read is a **system call** — a user→kernel→device
round trip with fixed overhead — so block size dominates:

```
  sequential read, by block size:
    4 KB    blocks : 0.10 GB/s     ← 131,072 syscalls to read 512 MB
    64 KB   blocks : 0.64 GB/s
    1 MB    blocks : 4.97 GB/s
    4 MB    blocks : 9.68 GB/s     ← 128 syscalls — ~100× faster
```

Same 512 MB read every time. Reading it in 4 KB sips pays the per-call cost
131,072 times and crawls at 0.10 GB/s; reading it in 4 MB gulps pays it 128 times
and flies at ~9.7 GB/s. *Amortizing fixed overhead over a bigger transfer* is the
whole game at the storage layer — and notice it's the same shape as every other
chapter: a fixed cost hurts only when you fail to overlap or batch around it.

---

## Chapter 11 — The whole chip, for real

The earlier chapters measured one core, or estimated the rest. `chipwide.c`
measures the *actual* whole-chip ceilings across all 18 cores — and the two
ceilings behave completely differently.

**Memory bandwidth saturates almost immediately:**

```
  threads :   GB/s
      1   :    104
      4   :    285
      6   :    300   ← maxed out
     18   :    300   ← more cores buy nothing
```

One core already pulls 104 GB/s — a *third* of the entire chip's bandwidth — and
just ~6 cores saturate the memory system at ~300 GB/s. Past that, adding cores
does nothing, because they're all drinking from one shared pipe.

**But compute keeps scaling all the way:**

```
  threads :   GFLOP/s
      1   :    140
      6   :    786
     18   :  1,898   ≈ 1.9 TFLOP/s
```

FLOPS climb nearly linearly to all 18 cores, hitting **~1.9 trillion floating-
point operations per second** — because each core does its math in its own
registers and isn't fighting for a shared resource.

This contrast is the most important practical lesson in the whole course:
**whether more cores help depends entirely on what you're bottlenecked on.**
Compute-bound work scales to all 18 cores; memory-bound work saturates at ~6 and
then flatlines. Throwing cores at a bandwidth-bound program is throwing them
away.

---

## Chapter 12 — The other processor: the GPU

Back in Chapter 8 we said CPU SIMD does 4–16 numbers per instruction, and that
the "thousands at once" machine is the GPU. Your chip has one: a **40-core GPU**,
~5,000 ALUs. `gpubench.m` (Metal, not C) puts the same stopwatch on it.

```
1. FP32 FMA peak        : 14.92 TFLOP/s          (7.9× the CPU's 1.9)
2. occupancy sweep:
     1,024 threads      :  1,169 GFLOP/s         (8% of peak — mostly idle)
     65,536             : 12,517
     4,194,304          : 14,915                 (full peak)
3. memory bandwidth     :    578 GB/s            (1.9× the CPU's 300)
4. FP16 (half) FMA      : 17.72 TFLOP/s          (1.2× FP32 here)
5. matrix multiply 2048²: naive 1,437 → tiled 2,918 GFLOP/s  (2.0×)
```

**The headline:** ~15 TFLOP/s, nearly 8× the CPU — that's the payoff of thousands
of lanes when the work is "the same math on a mountain of data." And 578 GB/s of
bandwidth, almost double the CPU, which reveals the chip's *true* memory ceiling:
the CPU couldn't issue requests fast enough to use it all (Chapter 11), but the
GPU can.

**The deepest lesson is the occupancy curve — it's why CPUs and GPUs exist as
two different things.** At 1,024 threads the GPU manages just 1.17 TFLOP/s,
*slower than the CPU*; it doesn't reach full speed until ~250,000 threads are in
flight. Here's why, and it ties the whole course together:

> A **CPU hides latency with cleverness** — pipelining, out-of-order execution,
> branch prediction, prefetching (Chapters 1–9). All that machinery keeps *one*
> thread busy, so a CPU is fast even single-threaded.
>
> A **GPU hides latency with sheer numbers** — it has almost none of that
> cleverness, but tens of thousands of threads ready to go, so when one stalls on
> memory it instantly runs another. Starve it of threads and it just stalls.

Two opposite bets on the same problem (memory is slow, Chapter 5). The CPU bets
on making one thread never wait; the GPU bets on always having another thread
that isn't waiting. That's the whole reason your machine has both.

The last two tests echo earlier chapters one final time. **FP16** gave only 1.2×
(not the textbook 2×) because this dependent-FMA loop is partly issue-bound, not
ALU-width-bound — the big half-precision wins come from dedicated matrix units we
didn't touch. And **matrix multiply** is the GPU's locality lesson: tiling 16×16
blocks into fast threadgroup memory (the GPU's manual L1, reused many times)
**doubles** throughput for identical math — yet even tiled it reaches only ~20%
of the 14.9 TFLOP/s peak. Peak is a ceiling; real code is the question. Exactly
where we started.

---

## The whole machine, in one picture

We started with the naive model — *fetch one instruction, do it, fetch the next*
— and measured every layer of reality stacked on top of it. Same operation, an
add, as we turn on each mechanism:

| level | mechanism | throughput | the trick |
|-------|-----------|------------|-----------|
| naive | one at a time | 4.5 G/s (one core) | the mental model |
| pipelined + superscalar | ~6 units, if independent | 21 G/s (one core) | Ch. 2–3 |
| + SIMD | 4–16 lanes × several units | 73 G adds/s · 140 GFLOP/s (one core) | Ch. 8, 11 |
| **× 18 CPU cores** | the whole CPU (measured) | **318 G adds/s · 1.9 TFLOP/s** | Ch. 4, 11 |
| **the 40-core GPU** | thousands of lanes (measured) | **14.9 TFLOP/s · 578 GB/s** | Ch. 12 |

The CPU and GPU are two opposite answers to "memory is slow": the CPU makes *one*
thread never wait (out-of-order, prediction, prefetch); the GPU keeps *thousands*
of threads ready so one is always runnable. The CPU is fast on anything; the GPU
is ~8× faster but only once you feed it ~250,000 threads — below that it's
*slower* than the CPU. Right tool, right job.

And the other axis — what it costs when the data *isn't* already in registers.
Every rung measured on this machine, with a human-scale column to make the gulf
felt (pretend one cycle takes one second):

| tier | size | latency | ≈ cycles | if 1 cycle = 1 second |
|------|------|---------|----------|------------------------|
| registers | < 1 KB | ~0.2 ns | ~1 | **1 second** |
| L1 | 128 KB | 0.9 ns | ~4 | 4 seconds |
| L2 | 16 MB | ~2–9 ns | ~10–40 | ~20 seconds |
| SLC | tens of MB | ~18–50 ns | ~80–230 | ~2 minutes |
| DRAM | 128 GB | 95 ns | ~425 | ~7 minutes |
| **SSD** (random) | terabytes | 71 µs | ~315,000 | **~3.6 days** |

A register access is *now*; a trip to RAM is a 7-minute warehouse run; a random
SSD access is like mail-ordering the data and waiting almost four days. The cache
hierarchy exists entirely to keep you near the top of that table — and the **TLB**
(Ch. 9) is a second ladder running alongside it for *addresses*, with its own
~200-cycle cliff you can fall off even when the data is in L1.

**Real performance is the product of two things, both measured here:**

1. **Independence (ILP)** — is there other work to overlap? Pins you to 1× when
   absent (a dependency chain), buys ~6–20× when present. It showed up *every
   chapter*: dependent vs. independent adds, blocked vs. interleaved layout,
   random vs. streaming loads, mispredict-overlap, the FMA ceiling.
2. **Locality** — is the data reachable cheaply? Up to ~100× between L1 and a
   random DRAM miss, and the prefetcher can erase the size penalty *entirely* if
   your access pattern is predictable.

Multiply them and the span is enormous — ~2,000× between best-case compute and a
random DRAM miss, and ~300,000× all the way down to a random SSD access. Which
end your program lands on is decided mostly by **data layout, access order, and
dependency structure**, not by how many instructions you write. Every mechanism
in the chip — caches, prefetchers, out-of-order execution, superscalar issue,
branch prediction, SIMD, address translation, 18 CPU cores and a 40-core GPU —
exists to claw back one of those gaps. And you can watch each one earn its keep
with a stopwatch.

---

## The programs

| file | chapter | question it answers |
|------|---------|---------------------|
| `cpuspeed.c`    | 1–3 | clock speed, and a first look at core width |
| `superscalar.c` | 2   | does one core do many adds at once? (with a dual-clock cross-check) |
| `widthtest.c`   | 3   | why instruction *layout* changes throughput |
| `threadscale.c` | 4   | scaling across 18 cores, and the plateau |
| `memlatency.c`  | 5   | the cache hierarchy, by pointer-chase latency |
| `memcompare.c`  | 6   | random vs. sequential access; the prefetcher; bandwidth |
| `branchpredict.c` | 7 | the sorted-array effect and the branchless fix |
| `branchperiod.c`  | 7 | proof it's the predictor (not the compiler), by period sweep |
| `plaincif.c`      | 7 | why we use inline asm, not `-O0` |
| `simd.c`        | 8   | one instruction, many numbers; GFLOP/s |
| `tlbtest.c`     | 9   | the address-translation cache (TLB) and page-walk cost |
| `disktest.c`    | 10  | the SSD; F_NOCACHE; syscall amortization by block size |
| `chipwide.c`    | 11  | real whole-chip peaks: bandwidth saturation, 1.9 TFLOP/s |
| `gpubench.m`    | 12  | the 40-core GPU (Metal): TFLOP/s, occupancy, FP16, matmul |

> **A note on honesty.** Several numbers here are *best-of-N* (the fastest run,
> to see the hardware's ceiling past scheduler noise); single honest runs come
> out a bit lower, and the text says which is which. The clock and "cycle"
> figures rest on the stated 1-add-per-cycle assumption. And these are all
> *friendly* microbenchmarks — no locks, little memory traffic, perfect
> independence where we wanted it. Real workloads are messier and live further
> from every ceiling. The point was never the exact numbers; it was to *see the
> mechanisms*, each one isolated, each one measured.
