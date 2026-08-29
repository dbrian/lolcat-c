/*
 * lolcat-c — cat with rainbow colors, optimized for throughput.
 *
 * Design notes (the short version; see README.md for the measurements):
 *
 *  - Every ANSI sequence is a build-time constant (src/tables.h). The hot loop
 *    never formats a number and never touches floating point.
 *  - Phase math is 32.32 fixed point: one add per character, one add per line.
 *  - A chunk that is pure ASCII with no ESC/tab/CR takes a fused path with no
 *    per-character branches and no bounds checks — one 16-byte store and one
 *    4-byte store, with the character ORed into the sequence's pad slot. The
 *    output buffer is sized up front from a single scan of the chunk, which is
 *    what lets the inner loop drop the capacity check.
 *  - Seekable input is split into fixed nominal chunks snapped to newline
 *    boundaries. That rule is a pure function of the file, so each worker
 *    preads and resolves its own slice with no shared reader thread.
 *  - A chunk's starting color depends on how many lines precede it, so each
 *    worker publishes its own newline count and picks up the running prefix
 *    from a chain any worker may advance. Counting overlaps with colorizing
 *    rather than serializing ahead of it.
 *  - Colorized chunks land in a ring of output slots that a writer thread
 *    drains in order, so a worker on a slow core doesn't stall the others.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "tables.h"

#define LOLCAT_VERSION "0.1.0"
#define LOLCAT_AUTHORS "Brian Gosnell <dbrian@gmail.com>"

/* ------------------------------------------------------------------ config */

typedef enum { MODE_TRUECOLOR, MODE_256, MODE_NONE } color_mode_t;

/* Everything the hot path needs, resolved once at startup. */
typedef struct {
    uint64_t phase0;    /* 32.32 fixed-point phase of the first character */
    uint64_t char_inc;  /* phase advance per character */
    uint64_t line_inc;  /* phase advance per line */
    color_mode_t mode;
    bool fast;          /* the branch-free fast paths apply */
} rt_t;

static const char *g_prog = "lolcat-c";

static void die(const char *what, const char *detail) {
    if (detail)
        fprintf(stderr, "%s: %s: %s\n", g_prog, what, detail);
    else
        fprintf(stderr, "%s: %s\n", g_prog, what);
    exit(1);
}

/* --------------------------------------------------------------- utilities */

static void write_all(int fd, const uint8_t *buf, size_t n) {
    while (n) {
        ssize_t w = write(fd, buf, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            die("write failed", strerror(errno));
        }
        buf += (size_t)w;
        n -= (size_t)w;
    }
}

/* What a single scan of a chunk tells the driver. */
typedef struct {
    size_t lines;  /* '\n' count — decides where this chunk's rainbow starts */
    size_t tabs;   /* tabs expand 1 -> 8, which the output bound must allow for */
    size_t conts;  /* UTF-8 continuation bytes: copied through, not colored */
    bool simple;   /* no ESC, tab or CR: only codepoint splitting to worry about */
    bool clean;    /* simple and pure ASCII: eligible for the fused path */
} scan_t;

/* Single pass over a chunk gathering everything the driver needs. The counts
 * accumulate in u8 lanes and are drained before they can wrap; the ESC/CR/
 * high-bit tests only ever need a sticky OR. */
static void scan_chunk(const uint8_t *p, size_t n, scan_t *s) {
    size_t i = 0, lines = 0, tabs = 0, conts = 0;
    unsigned special = 0, high = 0;
#if defined(__ARM_NEON)
    const uint8x16_t vnl = vdupq_n_u8('\n'), vtab = vdupq_n_u8('\t');
    const uint8x16_t vesc = vdupq_n_u8(0x1b), vcr = vdupq_n_u8('\r');
    const uint8x16_t vhi = vdupq_n_u8(0x80), vc0 = vdupq_n_u8(0xC0);
    while (i + 64 <= n) {
        /* Four 16-byte vectors share each counter, so drain at 63 blocks
         * (4 * 63 = 252) to keep the u8 lanes from wrapping. */
        size_t blocks = (n - i) / 64;
        if (blocks > 63) blocks = 63;
        uint8x16_t nacc = vdupq_n_u8(0), tacc = nacc, cacc = nacc, sbad = nacc, hbad = nacc;
        for (size_t k = 0; k < blocks; k++, i += 64) {
            for (int q = 0; q < 64; q += 16) {
                uint8x16_t v = vld1q_u8(p + i + q);
                uint8x16_t t = vceqq_u8(v, vtab);
                nacc = vsubq_u8(nacc, vceqq_u8(v, vnl));
                tacc = vsubq_u8(tacc, t);
                cacc = vsubq_u8(cacc, vceqq_u8(vandq_u8(v, vc0), vhi));
                sbad = vorrq_u8(sbad, vorrq_u8(t, vorrq_u8(vceqq_u8(v, vesc),
                                                           vceqq_u8(v, vcr))));
                hbad = vorrq_u8(hbad, vcgeq_u8(v, vhi));
            }
        }
        lines += vaddlvq_u8(nacc);
        tabs += vaddlvq_u8(tacc);
        conts += vaddlvq_u8(cacc);
        special |= vmaxvq_u8(sbad);
        high |= vmaxvq_u8(hbad);
    }
#endif
    for (; i < n; i++) {
        uint8_t b = p[i];
        lines += (b == '\n');
        tabs += (b == '\t');
        conts += ((b & 0xC0) == 0x80);
        special |= (b == 0x1b) | (b == '\t') | (b == '\r');
        high |= (b >= 0x80);
    }
    s->lines = lines;
    s->tabs = tabs;
    s->conts = conts;
    s->simple = (special == 0);
    s->clean = (special == 0) && (high == 0);
}

/* Newline scanner that keeps a 16-byte match mask in registers, so short lines
 * don't pay a libc call each. NEON has no movemask; vshrn gives four bits per
 * input byte, which is enough to locate matches. */
typedef struct {
    const uint8_t *cur, *end, *mbase;
    uint64_t mask;
} nls_t;

static inline void nls_init(nls_t *s, const uint8_t *p, size_t n) {
    s->cur = p;
    s->end = p + n;
    s->mbase = p;
    s->mask = 0;
}

static inline const uint8_t *nls_next(nls_t *s) {
#if defined(__ARM_NEON)
    for (;;) {
        if (s->mask) {
            int bit = __builtin_ctzll(s->mask);
            s->mask &= ~(0xFULL << (bit & ~3));
            return s->mbase + (bit >> 2);
        }
        if (s->cur + 16 > s->end) break;
        uint8x16_t v = vld1q_u8(s->cur);
        uint8x16_t c = vceqq_u8(v, vdupq_n_u8('\n'));
        uint64_t m = vget_lane_u64(
            vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(c), 4)), 0);
        s->mbase = s->cur;
        s->cur += 16;
        s->mask = m;
    }
#endif
    while (s->cur < s->end) {
        const uint8_t *p = s->cur++;
        if (*p == '\n') return p;
    }
    return NULL;
}

/* ------------------------------------------------------------ colorization */

/* Emit the TrueColor sequence for `idx`, leaving the cursor on the pad byte so
 * the caller's character overwrites it. Writes 20 bytes, advances 19. */
static inline uint8_t *emit_tc(uint8_t *o, unsigned idx) {
    memcpy(o, TC_HEAD[idx], 16);
    memcpy(o + 16, &TC_TAIL[idx], 4);
    return o + 19;
}

static inline uint8_t *emit_256(uint8_t *o, unsigned idx) {
    unsigned code = C256[idx];
    memcpy(o, A256[code], 12); /* fixed-width copy; only LEN bytes are kept */
    return o + A256_LEN[code];
}

#define PHASE_IDX(ph) ((unsigned)((ph) >> 32) & RAINBOW_MASK)

/*
 * General per-line colorizer: handles UTF-8, tab expansion and embedded ANSI
 * escapes. `TRUECOLOR` is a compile-time literal at both call sites so this
 * specializes into two branch-free-ish variants.
 *
 * Only codepoint-start bytes advance the phase, so a multi-byte character gets
 * one color rather than one per byte, and no escape is ever injected between a
 * lead byte and its continuations.
 */
static inline uint8_t *cz_line_impl(const uint8_t *b, size_t len, uint64_t ph,
                                    uint64_t inc, uint8_t *o, const int TRUECOLOR) {
    unsigned last = RAINBOW_SIZE; /* sentinel: no color emitted yet */
    size_t i = 0;
    while (i < len) {
        uint8_t c = b[i];

        if (c == 0x1b) {
            /* Pass the escape through untouched and forget the current color:
             * the sequence may well have changed it. */
            size_t j = i + 1;
            size_t limit = i + 1 + 200;
            if (limit > len) limit = len;
            while (j < limit) {
                uint8_t e = b[j++];
                if ((e >= 'A' && e <= 'Z') || (e >= 'a' && e <= 'z')) break;
            }
            memcpy(o, b + i, j - i);
            o += j - i;
            i = j;
            last = RAINBOW_SIZE;
            continue;
        }

        if (c == '\t') {
            for (int k = 0; k < 8; k++) {
                unsigned idx = PHASE_IDX(ph);
                if (idx != last) {
                    o = TRUECOLOR ? emit_tc(o, idx) : emit_256(o, idx);
                    last = idx;
                }
                *o++ = ' ';
                ph += inc;
            }
            i++;
            continue;
        }

        if (c < 0x80 || c >= 0xC0) {
            unsigned idx = PHASE_IDX(ph);
            if (idx != last) {
                o = TRUECOLOR ? emit_tc(o, idx) : emit_256(o, idx);
                last = idx;
            }
            ph += inc;
        }
        *o++ = c;
        i++;
    }
    *o++ = '\n';
    return o;
}

static uint8_t *cz_line_tc(const uint8_t *b, size_t len, uint64_t ph, uint64_t inc, uint8_t *o) {
    return cz_line_impl(b, len, ph, inc, o, 1);
}

static uint8_t *cz_line_256(const uint8_t *b, size_t len, uint64_t ph, uint64_t inc, uint8_t *o) {
    return cz_line_impl(b, len, ph, inc, o, 0);
}

/*
 * One colored ASCII character: a 16-byte store plus a 4-byte store, with the
 * character ORed into the sequence's zero pad slot so nothing is written twice.
 * No branch, no bounds check — the caller sized the buffer for the whole chunk.
 */
static inline uint8_t *put_ascii(uint8_t *o, unsigned idx, uint8_t c) {
#if defined(__ARM_NEON)
    vst1q_u8(o, vld1q_u8(TC_HEAD[idx]));
#else
    memcpy(o, TC_HEAD[idx], 16);
#endif
    uint32_t tail = TC_TAIL[idx] | ((uint32_t)c << 24);
    memcpy(o + 16, &tail, 4);
    return o + 20;
}

/*
 * Fast-path line colorizer, used when the chunk holds no ESC, tab or CR and
 * the color index advances at least once per character — so the sequence
 * always changes and there is nothing for a last-color check to save.
 *
 * With ASCII_ONLY the loop reduces to two stores per byte. Otherwise it also
 * splits UTF-8: a lead byte takes the color and its continuation bytes are
 * copied straight through, so a multi-byte character is never torn apart by an
 * escape sequence. Orphan continuations are copied without advancing the phase,
 * matching the general colorizer and the output-bound accounting.
 */
static inline uint8_t *cz_fast_line(const uint8_t *b, const uint8_t *e, uint64_t ph,
                                    uint64_t inc, uint8_t *o, const int ASCII_ONLY) {
    while (b < e) {
        uint8_t c = *b++;
        if (ASCII_ONLY || c < 0x80) {
            unsigned idx = PHASE_IDX(ph);
            ph += inc;
            o = put_ascii(o, idx, c);
        } else if ((c & 0xC0) == 0x80) {
            *o++ = c;
        } else {
            unsigned idx = PHASE_IDX(ph);
            ph += inc;
            o = emit_tc(o, idx);
            *o++ = c;
            while (b < e && (*b & 0xC0) == 0x80) *o++ = *b++;
        }
    }
    *o++ = '\n';
    return o;
}

/* Split a chunk into lines and run the fast-path colorizer over each. */
static inline uint8_t *cz_fast_chunk(const uint8_t *p, size_t n, uint64_t phase, uint64_t inc,
                                     uint64_t line_inc, uint8_t *o, bool is_final,
                                     const int ASCII_ONLY) {
    nls_t s;
    nls_init(&s, p, n);
    const uint8_t *ls = p, *q;
    while ((q = nls_next(&s)) != NULL) {
        o = cz_fast_line(ls, q, phase, inc, o, ASCII_ONLY);
        phase += line_inc;
        ls = q + 1;
    }
    if (is_final && ls < p + n) o = cz_fast_line(ls, p + n, phase, inc, o, ASCII_ONLY);
    return o;
}

static uint8_t *cz_fast_ascii(const uint8_t *p, size_t n, uint64_t ph, uint64_t inc,
                              uint64_t linc, uint8_t *o, bool fin) {
    return cz_fast_chunk(p, n, ph, inc, linc, o, fin, 1);
}

static uint8_t *cz_fast_utf8(const uint8_t *p, size_t n, uint64_t ph, uint64_t inc,
                             uint64_t linc, uint8_t *o, bool fin) {
    return cz_fast_chunk(p, n, ph, inc, linc, o, fin, 0);
}

/*
 * Upper bound on a chunk's colorized size.
 *
 * Every codepoint start costs at most 20 bytes (a 19-byte TrueColor sequence
 * plus the character sharing its pad slot; a 256-color sequence is shorter,
 * and an ESC byte is passed through at one byte). Continuation bytes and
 * newlines cost one each. A tab expands to eight colored spaces, so it needs
 * 140 bytes beyond the 20 already counted for it.
 *
 * Sizing this tightly rather than assuming 20 bytes per input byte matters:
 * these buffers are what the store traffic lands in, and on multibyte input
 * the loose bound made them roughly twice as large as they needed to be.
 */
static inline size_t out_bound(const scan_t *s, size_t n) {
    size_t starts = n - s->conts;
    return 20 * starts + s->conts + s->lines + 140 * s->tabs + 64;
}

/* Colorize one newline-aligned chunk. `is_final` means trailing bytes after
 * the last newline are a real last line rather than a partial one. */
static size_t colorize_chunk(const uint8_t *p, size_t n, uint64_t phase, const rt_t *rt,
                             const scan_t *sc, uint8_t *out, bool is_final) {
    if (rt->fast && sc->simple) {
        uint8_t *o = sc->clean ? cz_fast_ascii(p, n, phase, rt->char_inc, rt->line_inc, out, is_final)
                               : cz_fast_utf8(p, n, phase, rt->char_inc, rt->line_inc, out, is_final);
        return (size_t)(o - out);
    }

    uint8_t *o = out;
    size_t off = 0;
    while (off < n) {
        const uint8_t *q = memchr(p + off, '\n', n - off);
        if (!q) break;
        size_t end = (size_t)(q - p);
        size_t le = end;
        if (le > off && p[le - 1] == '\r') le--; /* CRLF: drop the CR */
        o = rt->mode == MODE_TRUECOLOR ? cz_line_tc(p + off, le - off, phase, rt->char_inc, o)
                                       : cz_line_256(p + off, le - off, phase, rt->char_inc, o);
        phase += rt->line_inc;
        off = end + 1;
    }
    if (is_final && off < n) {
        o = rt->mode == MODE_TRUECOLOR ? cz_line_tc(p + off, n - off, phase, rt->char_inc, o)
                                       : cz_line_256(p + off, n - off, phase, rt->char_inc, o);
    }
    return (size_t)(o - out);
}

/* ---------------------------------------------------------- parallel driver */

#define RESET_SEQ "\x1b[0m\x1b[39m\x1b[49m"

/*
 * Both handoffs below are "wait until a counter reaches my chunk index", and
 * every chunk pays one of each.
 *
 * The counter is a plain atomic so the already-satisfied case (the common one,
 * since a chunk takes far longer to colorize than its predecessor takes to
 * write) costs a single load and no lock at all, and the releasing side skips
 * the mutex whenever nobody is actually asleep. Waiters that do have to block
 * go straight to a condition variable rather than spinning: with a worker per
 * core, a spinning waiter steals the core from the worker it is waiting on,
 * and measured 4x worse.
 */

typedef struct {
    _Atomic size_t value;    /* monotonically increasing counter */
    _Atomic int sleepers;
    pthread_mutex_t mu;
    pthread_cond_t cv;
} gate_t;

static void gate_init(gate_t *g) {
    atomic_init(&g->value, 0);
    atomic_init(&g->sleepers, 0);
    pthread_mutex_init(&g->mu, NULL);
    pthread_cond_init(&g->cv, NULL);
}

static void gate_destroy(gate_t *g) {
    pthread_mutex_destroy(&g->mu);
    pthread_cond_destroy(&g->cv);
}

/* Block until the counter is at least `want`. */
static void gate_wait(gate_t *g, size_t want) {
    if (atomic_load_explicit(&g->value, memory_order_acquire) >= want) return;
    /* The increment and the re-check below are ordered against the releasing
     * side's store-then-read-sleepers, so a wakeup can't be lost between them. */
    pthread_mutex_lock(&g->mu);
    atomic_fetch_add(&g->sleepers, 1);
    while (atomic_load(&g->value) < want) pthread_cond_wait(&g->cv, &g->mu);
    atomic_fetch_sub(&g->sleepers, 1);
    pthread_mutex_unlock(&g->mu);
}

/* Publish a new counter value, waking sleepers only if there are any. */
static void gate_set(gate_t *g, size_t value) {
    atomic_store(&g->value, value);
    if (atomic_load(&g->sleepers) > 0) {
        pthread_mutex_lock(&g->mu);
        pthread_cond_broadcast(&g->cv);
        pthread_mutex_unlock(&g->mu);
    }
}

/*
 * Output slot ring. Chunk c colorizes into slot c % nslots and a dedicated
 * writer thread drains the slots in index order.
 *
 * The obvious alternative — let each worker write its own output when its turn
 * comes — makes every worker block until its predecessor has written. On a
 * machine with both performance and efficiency cores that is expensive: a
 * chunk that lands on an E-core takes several times longer, and every faster
 * worker behind it sits idle. (Measured: ~40% of all worker time was spent
 * blocked on that handoff.) With a slot ring, workers run ahead of a straggler
 * by up to nslots chunks and only block when the ring is genuinely full.
 */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    gate_t filled; /* reaches c+1 when chunk c's output is in this slot */
} slot_t;

typedef struct {
    int in_fd;
    size_t size;
    size_t chunk;     /* nominal input bytes per chunk */
    size_t nchunks;
    const rt_t *rt;
    int out_fd;

    _Atomic size_t next_chunk;

    /* Line-count chain. Every worker publishes its chunk's newline count, then
     * waits for the running prefix to reach its own index. Whoever holds the
     * lock advances the chain as far as the published counts allow. */
    uint64_t *count;
    bool *ready;
    uint64_t *prefix; /* prefix[c] = lines before chunk c; valid once chain > c */
    size_t chain;
    pthread_mutex_t chain_mu;
    gate_t chain_gate;

    slot_t *slots;
    size_t nslots;
    gate_t written;   /* number of chunks written; frees slot (written - nslots) */
} pool_t;

/* Read exactly [off, off+len) unless the file ends first. */
static size_t pread_all(int fd, uint8_t *buf, size_t len, size_t off) {
    size_t got = 0;
    while (got < len) {
        ssize_t r = pread(fd, buf + got, len - got, (off_t)(off + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            die("read failed", strerror(errno));
        }
        if (r == 0) break;
        got += (size_t)r;
    }
    return got;
}

/*
 * Chunk c nominally covers input bytes [c*chunk, (c+1)*chunk), snapped forward
 * to the newline that follows each end — so chunk c starts just past the first
 * '\n' at or after c*chunk, and ends just past the first '\n' at or after
 * (c+1)*chunk. That rule is a pure function of the file, so every worker can
 * resolve its own boundaries without talking to anyone else.
 *
 * A worker reads one window covering both boundaries. WINDOW_TAIL is the slack
 * past the nominal end where the terminating newline usually lives; lines
 * longer than that just grow the window. Chunks whose two boundaries collapse
 * to the same newline are empty, which is exactly right for a line spanning
 * several nominal chunks.
 */
#define WINDOW_TAIL 4096

typedef struct {
    uint8_t *buf;
    size_t cap, have;  /* have = valid bytes, representing [base, base+have) */
    size_t base;
} window_t;

static void window_fill(window_t *w, int fd, size_t size, size_t want) {
    if (w->base + want > size) want = size - w->base;
    if (want <= w->have) return;
    if (want > w->cap) {
        w->cap = want + want / 2;
        w->buf = (uint8_t *)realloc(w->buf, w->cap);
        if (!w->buf) die("out of memory", NULL);
    }
    w->have += pread_all(fd, w->buf + w->have, want - w->have, w->base + w->have);
}

/* First newline at or after window offset `from`, growing the window as needed.
 * Returns the offset just past it, or `have` if the file ends first. */
static size_t window_next_line(window_t *w, int fd, size_t size, size_t from) {
    size_t searched = from;
    for (;;) {
        if (searched < w->have) {
            const uint8_t *nl = memchr(w->buf + searched, '\n', w->have - searched);
            if (nl) return (size_t)(nl - w->buf) + 1;
            searched = w->have;
        }
        if (w->base + w->have >= size) return w->have;
        window_fill(w, fd, size, w->have + WINDOW_TAIL);
    }
}

static void *writer(void *arg) {
    pool_t *P = (pool_t *)arg;
    for (size_t c = 0; c < P->nchunks; c++) {
        slot_t *s = &P->slots[c % P->nslots];
        gate_wait(&s->filled, c + 1);
        write_all(P->out_fd, s->buf, s->len);
        gate_set(&P->written, c + 1);
    }
    return NULL;
}

static void *worker(void *arg) {
    pool_t *P = (pool_t *)arg;
    window_t w = {NULL, 0, 0, 0};

    for (;;) {
        size_t c = atomic_fetch_add_explicit(&P->next_chunk, 1, memory_order_relaxed);
        if (c >= P->nchunks) break;

        /* Claim this chunk's slot: it is free once the writer has drained the
         * chunk that used it a full ring ago. */
        slot_t *slot = &P->slots[c % P->nslots];
        if (c >= P->nslots) gate_wait(&P->written, c + 1 - P->nslots);

        w.base = c * P->chunk;
        w.have = 0;
        window_fill(&w, P->in_fd, P->size, P->chunk + WINDOW_TAIL);

        size_t start = c == 0 ? 0 : window_next_line(&w, P->in_fd, P->size, 0);
        size_t end = window_next_line(&w, P->in_fd, P->size,
                                      P->chunk < w.have ? P->chunk : w.have);

        const uint8_t *p = w.buf + start;
        size_t n = end - start;
        bool is_final = (w.base + end >= P->size);

        /* Touching the chunk here also warms it for the colorize pass below. */
        scan_t sc;
        scan_chunk(p, n, &sc);

        /* Publish this chunk's line count and fold in every prefix that the
         * published counts now allow. prefix[c] is written before the gate
         * advances past c, which is what makes the lock-free read below safe. */
        pthread_mutex_lock(&P->chain_mu);
        P->count[c] = sc.lines;
        P->ready[c] = true;
        size_t chain = P->chain;
        while (chain < P->nchunks && P->ready[chain]) {
            P->prefix[chain + 1] = P->prefix[chain] + P->count[chain];
            chain++;
        }
        if (chain != P->chain) {
            P->chain = chain;
            gate_set(&P->chain_gate, chain);
        }
        pthread_mutex_unlock(&P->chain_mu);

        gate_wait(&P->chain_gate, c); /* chain >= c means prefix[c] is final */
        uint64_t lines_before = P->prefix[c];

        size_t need = out_bound(&sc, n);
        if (need > slot->cap) {
            free(slot->buf);
            slot->buf = (uint8_t *)malloc(need);
            if (!slot->buf) die("out of memory", NULL);
            slot->cap = need;
        }

        uint64_t phase = P->rt->phase0 + P->rt->line_inc * lines_before;
        slot->len = colorize_chunk(p, n, phase, P->rt, &sc, slot->buf, is_final);
        gate_set(&slot->filled, c + 1);
    }
    free(w.buf);
    return NULL;
}

static int thread_count(void) {
    const char *env = getenv("LOLCAT_THREADS");
    if (env) {
        int v = atoi(env);
        if (v > 0) return v;
    }
    long nc = sysconf(_SC_NPROCESSORS_ONLN);
    if (nc < 1) nc = 1;
    if (nc > 8) nc = 8;
    return (int)nc;
}

/*
 * Nominal input bytes per chunk. Measured optimum on an M3: below this the
 * per-chunk syscalls and handoffs start to show, above it the colorized output
 * (roughly 20x the input) stops fitting comfortably in cache.
 */
static size_t chunk_bytes(void) {
    const char *env = getenv("LOLCAT_CHUNK");
    if (env) {
        long v = atol(env);
        if (v >= 1024) return (size_t)v;
    }
    return 32 * 1024;
}

/* Single-threaded: no chunking, no locks. Used for small inputs, where thread
 * setup would dominate, and whenever only one worker is requested. */
static void run_serial(const uint8_t *data, size_t size, const rt_t *rt, int fd) {
    scan_t sc;
    scan_chunk(data, size, &sc);
    size_t need = out_bound(&sc, size);
    uint8_t *buf = (uint8_t *)malloc(need);
    if (!buf) die("out of memory", NULL);
    size_t len = colorize_chunk(data, size, rt->phase0, rt, &sc, buf, true);
    write_all(fd, buf, len);
    free(buf);
}

static void run_parallel(int in_fd, size_t size, const rt_t *rt, int out_fd, int nthreads) {
    pool_t P;
    memset(&P, 0, sizeof P);
    P.in_fd = in_fd;
    P.size = size;
    P.rt = rt;
    P.out_fd = out_fd;
    P.chunk = chunk_bytes();
    P.nchunks = (size + P.chunk - 1) / P.chunk;
    atomic_init(&P.next_chunk, 0);
    P.count = (uint64_t *)calloc(P.nchunks + 1, sizeof *P.count);
    P.ready = (bool *)calloc(P.nchunks + 1, sizeof *P.ready);
    P.prefix = (uint64_t *)calloc(P.nchunks + 1, sizeof *P.prefix);
    if (!P.count || !P.ready || !P.prefix) die("out of memory", NULL);
    pthread_mutex_init(&P.chain_mu, NULL);
    gate_init(&P.chain_gate);
    gate_init(&P.written);

    if ((size_t)nthreads > P.nchunks) nthreads = (int)P.nchunks;
    /* Enough slack for every worker to hold a slot and still run ahead of a
     * straggler by roughly another full round. */
    P.nslots = (size_t)nthreads * 2 + 2;
    {
        const char *e = getenv("LOLCAT_SLOTS");
        if (e && atoi(e) > 0) P.nslots = (size_t)atoi(e);
    }
    if (P.nslots > P.nchunks) P.nslots = P.nchunks;
    P.slots = (slot_t *)calloc(P.nslots, sizeof *P.slots);
    if (!P.slots) die("out of memory", NULL);
    for (size_t i = 0; i < P.nslots; i++) gate_init(&P.slots[i].filled);

    pthread_t *tid = (pthread_t *)malloc((size_t)nthreads * sizeof *tid);
    if (!tid) die("out of memory", NULL);
    pthread_t wtid;
    if (pthread_create(&wtid, NULL, writer, &P) != 0) die("pthread_create failed", NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tid[i], NULL, worker, &P) != 0) die("pthread_create failed", NULL);
    }
    for (int i = 0; i < nthreads; i++) pthread_join(tid[i], NULL);
    pthread_join(wtid, NULL);

    for (size_t i = 0; i < P.nslots; i++) {
        gate_destroy(&P.slots[i].filled);
        free(P.slots[i].buf);
    }
    free(P.slots);
    free(tid);
    pthread_mutex_destroy(&P.chain_mu);
    gate_destroy(&P.chain_gate);
    gate_destroy(&P.written);
    free(P.count);
    free(P.ready);
    free(P.prefix);
}

/* ------------------------------------------------------- streaming (stdin) */

/*
 * Pipes and character devices can't be mapped, and their length isn't known
 * ahead of time, so a reader thread assembles newline-aligned chunks into
 * recycled buffers and hands them to the same worker pool. The reader also
 * counts newlines: a pipe is producer-bound anyway, so there is nothing to
 * gain from deferring that to the workers.
 */
#define SQ_SLOTS 8

typedef struct {
    uint8_t *buf;
    size_t len;
    uint64_t phase;
    bool is_final;
} sjob_t;

typedef struct {
    sjob_t slot[SQ_SLOTS];
    size_t head, tail; /* monotonically increasing sequence numbers */
    bool done;
    pthread_mutex_t mu;
    pthread_cond_t not_empty, not_full;

    const rt_t *rt;
    int out_fd;
    gate_t turn;   /* reaches seq once chunk seq-1 has been written */
} squeue_t;

static void *stream_worker(void *arg) {
    squeue_t *Q = (squeue_t *)arg;
    uint8_t *out = NULL;
    size_t cap = 0;

    for (;;) {
        pthread_mutex_lock(&Q->mu);
        while (Q->head == Q->tail && !Q->done) pthread_cond_wait(&Q->not_empty, &Q->mu);
        if (Q->head == Q->tail && Q->done) {
            pthread_mutex_unlock(&Q->mu);
            break;
        }
        size_t seq = Q->head++;
        sjob_t job = Q->slot[seq % SQ_SLOTS];
        pthread_cond_signal(&Q->not_full);
        pthread_mutex_unlock(&Q->mu);

        scan_t sc;
        scan_chunk(job.buf, job.len, &sc);
        size_t need = out_bound(&sc, job.len);
        if (need > cap) {
            free(out);
            out = (uint8_t *)malloc(need);
            if (!out) die("out of memory", NULL);
            cap = need;
        }
        size_t len = colorize_chunk(job.buf, job.len, job.phase, Q->rt, &sc, out, job.is_final);

        gate_wait(&Q->turn, seq);
        write_all(Q->out_fd, out, len);
        gate_set(&Q->turn, seq + 1);

        free(job.buf);
    }
    free(out);
    return NULL;
}

static void run_stream(int in_fd, const rt_t *rt, int out_fd, int nthreads) {
    const size_t CHUNK = chunk_bytes();
    const size_t CAP = CHUNK * 2;

    squeue_t Q;
    memset(&Q, 0, sizeof Q);
    Q.rt = rt;
    Q.out_fd = out_fd;
    pthread_mutex_init(&Q.mu, NULL);
    pthread_cond_init(&Q.not_empty, NULL);
    pthread_cond_init(&Q.not_full, NULL);
    gate_init(&Q.turn);

    pthread_t *tid = (pthread_t *)malloc((size_t)nthreads * sizeof *tid);
    if (!tid) die("out of memory", NULL);
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tid[i], NULL, stream_worker, &Q) != 0)
            die("pthread_create failed", NULL);
    }

    size_t cap = CAP;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) die("out of memory", NULL);
    size_t len = 0;
    uint64_t phase = rt->phase0;
    bool eof = false;

    while (!eof) {
        /* A line longer than the buffer would otherwise leave no room to read
         * into, and a zero-length read looks exactly like EOF. */
        if (len == cap) {
            cap *= 2;
            buf = (uint8_t *)realloc(buf, cap);
            if (!buf) die("out of memory", NULL);
        }
        ssize_t r = read(in_fd, buf + len, cap - len);
        if (r < 0) {
            if (errno == EINTR) continue;
            die("read failed", strerror(errno));
        }
        if (r == 0) {
            eof = true;
        } else {
            len += (size_t)r;
            if (len < CHUNK) continue;
        }

        /* Split at the last newline; the tail carries into the next buffer. */
        size_t take = len;
        if (!eof) {
            uint8_t *q = NULL;
            for (size_t k = len; k > 0; k--) {
                if (buf[k - 1] == '\n') { q = buf + k - 1; break; }
            }
            if (!q) continue; /* no newline yet — keep filling */
            take = (size_t)(q - buf) + 1;
        }
        if (take == 0) break;

        size_t carry = len - take;
        size_t ncap = carry + CHUNK > CAP ? carry + CHUNK : CAP;
        uint8_t *next = (uint8_t *)malloc(ncap);
        if (!next) die("out of memory", NULL);
        if (carry) memcpy(next, buf + take, carry);
        cap = ncap;

        scan_t rsc;
        scan_chunk(buf, take, &rsc);

        pthread_mutex_lock(&Q.mu);
        while (Q.tail - Q.head >= SQ_SLOTS) pthread_cond_wait(&Q.not_full, &Q.mu);
        Q.slot[Q.tail % SQ_SLOTS] = (sjob_t){buf, take, phase, eof};
        Q.tail++;
        pthread_cond_signal(&Q.not_empty);
        pthread_mutex_unlock(&Q.mu);

        phase += rt->line_inc * rsc.lines;
        buf = next;
        len = carry;
    }

    if (len > 0) {
        pthread_mutex_lock(&Q.mu);
        while (Q.tail - Q.head >= SQ_SLOTS) pthread_cond_wait(&Q.not_full, &Q.mu);
        Q.slot[Q.tail % SQ_SLOTS] = (sjob_t){buf, len, phase, true};
        Q.tail++;
        pthread_cond_signal(&Q.not_empty);
        pthread_mutex_unlock(&Q.mu);
    } else {
        free(buf);
    }

    pthread_mutex_lock(&Q.mu);
    Q.done = true;
    pthread_cond_broadcast(&Q.not_empty);
    pthread_mutex_unlock(&Q.mu);

    for (int i = 0; i < nthreads; i++) pthread_join(tid[i], NULL);
    free(tid);
}

/* Plain copy, no color: matches `cat`. */
static void run_nocolor(int in_fd, int out_fd) {
    size_t cap = 256 * 1024;
    uint8_t *buf = (uint8_t *)malloc(cap);
    if (!buf) die("out of memory", NULL);
    for (;;) {
        ssize_t r = read(in_fd, buf, cap);
        if (r < 0) {
            if (errno == EINTR) continue;
            die("read failed", strerror(errno));
        }
        if (r == 0) break;
        write_all(out_fd, buf, (size_t)r);
    }
    free(buf);
}

/* ------------------------------------------------------- color mode detect */

static bool env_true(const char *name) { return getenv(name) != NULL; }

static bool contains_ci(const char *hay, const char *needle) {
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; p++) {
        size_t k = 0;
        while (k < nl && p[k] && (char)tolower((unsigned char)p[k]) == needle[k]) k++;
        if (k == nl) return true;
    }
    return false;
}

static color_mode_t detect_color_support(bool force) {
    if (env_true("NO_COLOR")) return MODE_NONE;
    if (force) return MODE_TRUECOLOR;

    const char *fc = getenv("FORCE_COLOR");
    if (fc) {
        if (strcmp(fc, "0") == 0) return MODE_NONE;
        return (strcmp(fc, "3") == 0) ? MODE_TRUECOLOR : MODE_256;
    }

    if (!isatty(STDOUT_FILENO)) return MODE_NONE;

    const char *term = getenv("TERM");
    if (term && (strcasecmp(term, "dumb") == 0 || strcasecmp(term, "unknown") == 0))
        return MODE_NONE;

    const char *ct = getenv("COLORTERM");
    const char *tp = getenv("TERM_PROGRAM");
    if (contains_ci(ct, "truecolor") || contains_ci(ct, "24bit") ||
        contains_ci(tp, "iterm") || contains_ci(tp, "wezterm") || contains_ci(tp, "warp") ||
        contains_ci(tp, "alacritty") || contains_ci(tp, "ghostty") ||
        contains_ci(tp, "apple_terminal") ||
        env_true("WT_SESSION") || env_true("VSCODE_INJECTION") ||
        contains_ci(term, "xterm-kitty") || contains_ci(term, "alacritty") ||
        contains_ci(term, "wezterm") || contains_ci(term, "ghostty") ||
        contains_ci(term, "konsole") || contains_ci(term, "gnome") ||
        contains_ci(term, "vte") || contains_ci(term, "foot") || contains_ci(term, "iterm"))
        return MODE_TRUECOLOR;

    return MODE_256;
}

/* ------------------------------------------------------------------- main */

/* Knuth multiplicative hash of the pid: a per-run starting hue with no syscall
 * and no RNG state. Matches lolcat-ultra so the two paint the same rainbow for
 * a given pid. LOLCAT_OFFSET pins it for tests. */
static double random_offset(void) {
    const char *env = getenv("LOLCAT_OFFSET");
    if (env) return atof(env);
    uint32_t hash = (uint32_t)getpid() * 2654435769u;
    return (double)hash / (double)UINT32_MAX * 1000.0;
}

static void rt_init(rt_t *rt, double frequency, double spread, double offset, color_mode_t mode) {
    /* scale maps a rainbow position to a table index; s folds in the 32-bit
     * fixed-point shift so every per-character update is an integer add. */
    double scale = (double)RAINBOW_SIZE * (frequency / (2.0 * M_PI));
    double s = scale * 4294967296.0;
    rt->phase0 = (uint64_t)(offset * s);
    rt->char_inc = (uint64_t)((1.0 / spread) * s);
    rt->line_inc = (uint64_t)(spread * s);
    rt->mode = mode;
    /* Below one index step per character the sequence can repeat, and the
     * last-color check in the general path is what avoids emitting it twice. */
    rt->fast = (mode == MODE_TRUECOLOR) && (rt->char_inc >= (1ULL << 32));
}

static void print_rainbow(const char *text) {
    rt_t rt;
    rt_init(&rt, 0.04, 4.0, random_offset(), MODE_TRUECOLOR);
    run_serial((const uint8_t *)text, strlen(text), &rt, STDOUT_FILENO);
    write_all(STDOUT_FILENO, (const uint8_t *)RESET_SEQ, sizeof RESET_SEQ - 1);
}

static void print_help(void) {
    char buf[1024];
    snprintf(buf, sizeof buf,
             "cat with rainbow colors\n"
             "\n"
             "Usage: %s [OPTIONS] [FILE]\n"
             "\n"
             "Arguments:\n"
             "  [FILE]  input file\n"
             "\n"
             "Options:\n"
             "  -f, --frequency <FREQUENCY>  Color change frequency [default: 0.04]\n"
             "  -s, --spread <SPREAD>        Rainbow spread [default: 4.0]\n"
             "  -F, --force                  Force color even when stdout is not a tty\n"
             "  -h, --help                   Print help\n"
             "  -v, --version                Print version\n",
             g_prog);
    print_rainbow(buf);
}

static double parse_double(const char *arg, const char *val) {
    char *end = NULL;
    double d = strtod(val, &end);
    if (end == val || *end != '\0') {
        fprintf(stderr, "%s: invalid value '%s' for '%s': expected a floating point number\n",
                g_prog, val, arg);
        exit(1);
    }
    return d;
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0]) g_prog = argv[0];

    const char *path = NULL;
    double frequency = 0.04, spread = 4.0;
    bool force = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            print_help();
            return 0;
        }
        if (strcmp(a, "-v") == 0 || strcmp(a, "--version") == 0) {
            char buf[256];
            snprintf(buf, sizeof buf, "lolcat-c %s\nAuthors: %s\n", LOLCAT_VERSION, LOLCAT_AUTHORS);
            print_rainbow(buf);
            return 0;
        }
        if (strcmp(a, "-F") == 0 || strcmp(a, "--force") == 0) {
            force = true;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--frequency") == 0) {
            if (++i >= argc) die("missing value for '-f'", NULL);
            frequency = parse_double(a, argv[i]);
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--spread") == 0) {
            if (++i >= argc) die("missing value for '-s'", NULL);
            spread = parse_double(a, argv[i]);
        } else if (a[0] == '-' && a[1] != '\0') {
            fprintf(stderr, "%s: unknown option: %s\n", g_prog, a);
            return 1;
        } else {
            if (path) die("unexpected argument: only one input file is allowed", NULL);
            path = a;
        }
    }

    if (!(frequency > 0.0) || !isfinite(frequency)) {
        fprintf(stderr, "%s: invalid frequency: %g\n", g_prog, frequency);
        return 1;
    }
    if (!(spread > 0.0) || !isfinite(spread)) {
        fprintf(stderr, "%s: invalid spread: %g\n", g_prog, spread);
        return 1;
    }

    color_mode_t mode = detect_color_support(force);

    int in_fd = STDIN_FILENO;
    if (path) {
        in_fd = open(path, O_RDONLY);
        if (in_fd < 0) {
            fprintf(stderr, "%s: %s: %s\n", g_prog, path, strerror(errno));
            return 1;
        }
    }

    /* Regular files are seekable, so workers can pread their own slices in
     * parallel. Anything else (pipe, tty, character device) has to be streamed
     * by a single reader. */
    struct stat st;
    bool seekable = fstat(in_fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
    size_t size = seekable ? (size_t)st.st_size : 0;

    if (mode == MODE_NONE) {
        run_nocolor(in_fd, STDOUT_FILENO);
        return 0;
    }

    rt_t rt;
    rt_init(&rt, frequency, spread, random_offset(), mode);

    int nthreads = thread_count();
    if (!seekable) {
        run_stream(in_fd, &rt, STDOUT_FILENO, nthreads);
    } else if (size < 256 * 1024) {
        /* run_serial buffers the whole colorized result, which is ~20x the
         * input, so it is only for inputs small enough that chunking and
         * thread setup would cost more than they save. */
        uint8_t *all = (uint8_t *)malloc(size);
        if (!all) die("out of memory", NULL);
        size_t got = pread_all(in_fd, all, size, 0);
        run_serial(all, got, &rt, STDOUT_FILENO);
        free(all);
    } else {
        run_parallel(in_fd, size, &rt, STDOUT_FILENO, nthreads);
    }

    write_all(STDOUT_FILENO, (const uint8_t *)RESET_SEQ, sizeof RESET_SEQ - 1);
    return 0;
}
