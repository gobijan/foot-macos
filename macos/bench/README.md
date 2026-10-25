# Benchmarks

`make bench` measures foot against kitty and Ghostty (those of them
that are installed, in `/Applications` or `~/Applications`), on this
Mac. The latest results are in [results.md](results.md), and summed up
in the [README](../../.github/README.md#performance).

```sh
make bench                    # ~10 minutes; the results go to build-release/bench/<date>/
make bench ARGS=--save        # also to macos/bench/, and the summary to .github/README.md
make bench ARGS=--help        # all options
```

The terminals' windows open on screen, one after the other. Don't type
into them, and leave the Mac alone until the run is done: everything
else running competes for the same CPU.

## Comparing two builds of foot

```sh
make bench ARGS="--terminals foot --only throughput \
    --foot before=/Applications/foot.app/Contents/MacOS/foot \
    --foot after=build-release/macos/foot.app/Contents/MacOS/foot"
```

## How it's measured

Each terminal runs with its default configuration, except for the font
(Menlo, 14 points) and the window's size, which are the same for all.
The defaults differ in other ways, and those differences are part of
the results: kitty and Ghostty blink the cursor, for example, foot
doesn't.

**Throughput.** [`tbench`](tbench.c) runs inside the terminal, and
writes each workload to it (16-64 MiB), then a DA1 query (`CSI c`). The
time stops when the terminal's answer arrives, i.e. once it has
processed all of the output before the query. The workloads:

| Workload | |
|---|---|
| ASCII text | lines of printable ASCII, of random lengths |
| Unicode text | accents, CJK (double width), emoji, box drawing, a combining character |
| Colored cells | full screens, each cell with its own 24-bit foreground and background color |
| Scrolling | short lines (`y`, like `yes`), i.e. scrolling the whole screen |
| Scrolling region | the same, in a scrolling region (like a pager, or vim) |
| Cursor motion | a character at a random position, again and again |

Each terminal is launched 3 times for each window size, alternating
with the others, and runs each workload 3 times per launch; the result
is the median of those 9 runs. The output is the same for every run,
and depends only on the window's size: when a terminal's window turns
out smaller than asked for (e.g. it doesn't fit on the screen), its
results are marked as not comparable.

**Startup.** The time from launching the terminal's executable until
the command it runs starts (the median of 5 launches, after one that
warms up the caches). This includes creating the window, but not
necessarily its first frame.

**Idle window.** 5 seconds after it opened, for 30 seconds: the CPU
time, the wakeups (`proc_pid_rusage()`'s idle and interrupt wakeups),
and the memory footprint (what Activity Monitor shows as Memory).

**Conditions.** While the benchmark runs, the Mac doesn't sleep
(`caffeinate`). Before each measurement, it waits until the screen is
unlocked, and until no other process uses more than 40% of a core
(`--force` skips that, `--ignore` leaves out a process that is stuck at
100%); a measurement during which the screen got locked is repeated.
When the display changes (e.g. it's switched off, and macOS falls back
to a 1920x1080 one), the benchmark stops. The results record the Mac,
macOS, the display, the power source, the terminals' versions, the load
average, and what else kept the CPU busy.

Only compare results from the same display: while output keeps coming
in, foot draws its frames at the display's refresh rate, so on a 60 Hz
display it processes output faster than on a 144 Hz one.

## What it doesn't measure

Input latency (from a key press to the frame that shows it) needs
either a camera, or hooks into each terminal; neither is here. The
throughput workloads measure how fast a terminal processes output, not
how many frames it shows meanwhile.
