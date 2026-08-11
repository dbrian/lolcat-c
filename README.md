# lolcat-c

A C implementation of [lolcat](https://github.com/busyloop/lolcat), optimized for
throughput. Reads from stdin or a file and prints rainbow-colored text.

It is a port of [lolcat-ultra](https://github.com/dbrian/lolcat-ultra) (Rust) built
to the same specification, and produces **byte-for-byte identical output** — same
rainbow table, same fixed-point phase math, same escape sequences. `make test`
verifies that against the real `lolcat-ultra` binary on every run.

## Performance

On an Apple M3 (4 performance + 4 efficiency cores), reading pre-generated files
with output to `/dev/null`:

| command                | corpus | mean    | Mlines/s | MB/s   |
|------------------------|--------|---------|----------|--------|
| `cat` (I/O floor)      | ascii  | 0.009s  | 1160.9   | 11609  |
| **lolcat-c**           | ascii  | 0.028s  | 352.3    | 3523   |
| **lolcat-c**           | utf8   | 0.024s  | 89.0     | 4048   |
| lolcat-ultra (Rust)    | ascii  | 0.040s  | 252.5    | 2525   |
| lolcat-ultra (Rust)    | utf8   | 0.035s  | 60.1     | 2735   |
| Ruby lolcat (busyloop) | ascii  | 2.191s  | 0.02     | 0.2    |
| Ruby lolcat (busyloop) | utf8   | 7.110s  | 0.01     | 0.3    |

**1.40x faster than lolcat-ultra on ASCII, 1.48x on UTF-8** (and ~15,000x /
~12,600x the original Ruby). The ASCII corpus is 10M lines of `test line`
(~100 MB); the UTF-8 corpus is ~2.1M mixed multibyte lines. Ruby runs on a
50k-line slice of each and is compared by throughput. Reproduce with `make bench`.

### Versus `cat`

Every character gets its own TrueColor escape sequence, so 100 MB of input
becomes 1.81 GB of output. That 18x expansion, not the colorizing, is what
separates lolcat-c from `cat`:

| sink        | cat     | lolcat-c | ratio       | bytes written        |
|-------------|---------|----------|-------------|----------------------|
| `/dev/null` | 8.8 ms  | 28.5 ms  | 3.2x slower | 100 MB vs 1810 MB    |
| pipe to `wc -c` | 19.6 ms | 229.6 ms | 11.7x slower | 100 MB vs 1810 MB |

Writes to `/dev/null` are discarded without being copied, so that row measures
the cost of *generating* the output: lolcat-c produces 18x the bytes for 3.2x
the time. The pipe row measures actually *moving* them, and there the output
volume dominates completely — 350 ms of the 230 ms wall time is system time
spread across threads.

Per byte delivered, lolcat-c is faster than `cat` either way (63.6 GB/s vs
11.4 GB/s to `/dev/null`, 7.9 GB/s vs 5.1 GB/s through the pipe). The rainbow
itself is close to free; paying for it means paying to move 18x the data.

## Design

### Everything constant is computed at build time

`tools/gentables.c` runs during the build and emits `src/tables.h`: the
2048-entry rainbow table, the TrueColor escape for every entry, the 256-color
palette index for every entry, and the 256-color escape for every palette index.
The hot loop never formats a number, and after startup there is no floating
point anywhere.

TrueColor sequences are fixed-width — `\x1b[38;2;RRR;GGG;BBBm`, 19 bytes with
zero-padded decimals — and are stored split into a 16-byte head and a 4-byte
tail whose last byte is a zero pad. Emitting a colored ASCII character is then
one 16-byte store plus one 4-byte store, with the character ORed into the pad
slot. Nothing is written twice and nothing is misaligned.

The trig recurrence and the saturating float-to-byte rounding match
lolcat-ultra's `build.rs` exactly, which is what makes the generated tables
bit-identical.

### Fixed-point phase

The rainbow position is a 32.32 fixed-point accumulator. Per character it takes
one integer add; per line, one more. The table index is a shift and a mask.

### Three colorizing paths, chosen by one scan

Each chunk is scanned once (NEON, counting newlines, tabs and UTF-8
continuation bytes while OR-ing together the ESC/CR/high-bit tests), and the
result picks the path:

- **pure ASCII, no ESC/tab/CR** — two stores per character, no branches.
- **UTF-8, no ESC/tab/CR** — same, plus continuation bytes copied through so a
  multi-byte character takes one color instead of one per byte.
- **anything else** — the general path: tab expansion, embedded ANSI escapes
  passed through untouched, CRLF handling, 256-color and no-color modes.

That same scan also gives an exact-enough upper bound on the chunk's output
size, so the buffer is sized once up front and the inner loops carry no capacity
check at all.

### Parallelism without a serial reader

Chunk `c` covers input bytes `[c*chunk, (c+1)*chunk)`, snapped forward to the
newline following each end. That rule is a pure function of the file, so every
worker `pread`s and resolves its own slice — there is no reader thread to
bottleneck on, and boundaries never need to be agreed.

A chunk's starting color depends on how many lines precede it. Rather than
counting the whole file up front, each worker publishes its own chunk's newline
count and then picks up the running prefix from a chain that any worker may
advance. Counting overlaps with colorizing instead of serializing ahead of it.

Colorized chunks go into a ring of output slots that a dedicated writer thread
drains in index order.

### Things that were tried and lost

- **mmap instead of pread.** Faulting 100 MB into the address space costs ~5 ms
  single-threaded, and *more* when eight threads fault concurrently and contend
  on the VM map lock. Reading through `pread` into small recycled buffers is
  faster despite the copy.
- **Workers writing their own output in turn.** Simpler, but every worker blocks
  until its predecessor has written. On a machine with both performance and
  efficiency cores a chunk that lands on an E-core takes several times longer
  and everyone behind it idles — profiling showed ~40% of all worker time spent
  blocked on that handoff. Hence the slot ring.
- **Spinning instead of sleeping on the ordering handoffs.** With one worker per
  core, a spinning waiter steals the core from the worker it is waiting on.
  Measured 4x worse.
- **A shared condition variable for the handoffs.** Broadcasting to every worker
  on every chunk; the wakeup storm dominated the run at small chunk sizes.
- **Larger chunks.** Fewer syscalls and handoffs, but the colorized output is
  ~20x the input, so the buffers stop fitting in cache. 32 KB measured best.

## Building

```bash
make build     # -> ./lolcat-c
make test      # correctness suite (+ conformance vs lolcat-ultra if built)
make bench     # throughput vs lolcat-ultra and Ruby lolcat
make bench-quick   # skip the slow Ruby comparison
```

Requires a C11 compiler with pthreads. NEON is used where available and there
are portable fallbacks for everything else. `hyperfine` and `python3` are needed
for `make bench`; the conformance test needs a built `lolcat-ultra` at
`../lolcat-ultra/target/release/lolcat-ultra` (override with `ULTRA=`).

## Usage

```
cat with rainbow colors

Usage: lolcat-c [OPTIONS] [FILE]

Arguments:
  [FILE]  input file

Options:
  -f, --frequency <FREQUENCY>  Color change frequency [default: 0.04]
  -s, --spread <SPREAD>        Rainbow spread [default: 4.0]
  -F, --force                  Force color even when stdout is not a tty
  -h, --help                   Print help
  -v, --version                Print version
```

Color support is detected from `NO_COLOR`, `FORCE_COLOR`, `TERM`, `COLORTERM`
and `TERM_PROGRAM`, matching lolcat-ultra's rules.

### Tuning and test knobs

| variable         | effect                                                        |
|------------------|---------------------------------------------------------------|
| `LOLCAT_THREADS` | worker count (default: online CPUs, capped at 8)              |
| `LOLCAT_CHUNK`   | nominal input bytes per chunk (default: 32768)                |
| `LOLCAT_SLOTS`   | output ring slots (default: 2 x threads + 2)                  |
| `LOLCAT_OFFSET`  | pin the starting hue; used by the tests to make runs comparable |

Without `LOLCAT_OFFSET` the starting hue comes from a Knuth hash of the pid, so
each run paints a different rainbow — same as lolcat-ultra.

## Correctness

`make test` runs 34 checks:

- **Text preservation** — stripping the ANSI sequences returns the input, for
  ASCII, multibyte UTF-8, emoji, curly quotes, tabs, blank lines, CRLF, lone CR,
  all printable ASCII, and lines longer than any internal buffer.
- **Invariance** — output must not depend on thread count, chunk size, or
  whether the input arrived by `pread` or through a pipe.
- **Conformance** — byte-for-byte agreement with `lolcat-ultra`. Both tools
  derive their hue from their own pid, so the test launches lolcat-ultra through
  `sh -c 'echo $$; exec ...'` to capture its pid and replays lolcat-c with the
  matching offset.

## License

MIT
