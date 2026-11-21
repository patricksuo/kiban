/*
 * bench core (portable): see bench_core.h. Kernels are noinline and take their sizes and inputs from a context the
 * optimiser cannot see through; dependent chains carry an empty asm (OPQ) so the compiler cannot fold steps, and loops
 * over memory carry a memory barrier where the compiler could otherwise hoist a pure call or turn the loop into one
 * memcpy. OPQ and BARRIER emit no instructions.
 */
#include "bench_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NOINL __attribute__((noinline))
#define OPQ(x) __asm__ volatile("" : "+r"(x))
#define BARRIER() __asm__ volatile("" ::: "memory")
#define MAXREPS 31
#define KB 1024u
#define MB (1024u * 1024u)

const uint8_t *bench_ro_region;
size_t bench_ro_size;
const char *bench_ro_name = "rodata";

static volatile uint32_t s_sink;
static volatile uint32_t s_seed = 0x2545F491u;
static volatile float s_fa = 1.0000001f, s_fb = 1e-7f;
static volatile double s_da = 1.0000001, s_db = 1e-7;

void bench_sink(uint32_t v) { s_sink = s_sink * 31u + v; }

static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return *s = x;
}

/* ---- harness ---------------------------------------------------------------------------------------------------- */

typedef struct {
    const bench_env *e;
    const uint8_t *src;
    uint8_t *dst;
    size_t n;     /* iterations / bytes / count, per kernel */
    size_t m;     /* passes / depth / stride / class / chunk, per kernel */
    size_t q;     /* memcpy: source size */
    void *p;
    const char *const *names;
    const char *const *names2;
} kc;

typedef uint32_t (*kfn)(kc *c);

static int reps_of(const bench_env *e) { return e->reps < 3 ? 3 : e->reps > MAXREPS ? MAXREPS : e->reps; }

static uint32_t run(const bench_env *e, int quiet, kfn fn, kc *c, void (*prep)(kc *), uint32_t *t)
{
    uint32_t x = 0;
    const int reps = reps_of(e);
    for (int r = 0; r < reps; r++) {
        if (prep) prep(c);
        if (quiet && e->quiet_begin) e->quiet_begin();
        const uint32_t t0 = e->ticks();
        const uint32_t v = fn(c);
        const uint32_t t1 = e->ticks();
        if (quiet && e->quiet_end) e->quiet_end();
        t[r] = t1 - t0;
        x = x * 31u + v;
    }
    bench_sink(x);
    if (e->pause) e->pause();
    return x;
}

void bench_report(const bench_env *e, const char *suite, const char *name, const char *extra, double ops,
                  const char *unit, uint32_t *ticks, uint32_t chk)
{
    const int reps = reps_of(e);
    uint32_t s[MAXREPS];
    memcpy(s, ticks, sizeof(uint32_t) * (size_t)reps);
    for (int i = 1; i < reps; i++) /* insertion sort */
        for (int j = i; j > 0 && s[j - 1] > s[j]; j--) {
            const uint32_t t = s[j];
            s[j] = s[j - 1];
            s[j - 1] = t;
        }
    if (ops <= 0) ops = 1;
    const double mn = s[0] / ops, md = s[reps / 2] / ops, mx = s[reps - 1] / ops;
    const double spread = s[0] ? 100.0 * (double)(s[reps - 1] - s[0]) / (double)s[0] : 0.0;
    const double ns_med = md / e->ticks_per_ns;
    char bw[40] = "";
    if (strcmp(unit, "B") == 0 && ns_med > 0) snprintf(bw, sizeof bw, "\tmb_s=%.2f", 1000.0 / ns_med);
    if (e->ticks_are_cycles)
        bench_emit("suite=%s\tname=%s%s%s\tunit=%s\tops=%.0f\treps=%d\tcyc_min=%.3f\tcyc_med=%.3f\tcyc_max=%.3f"
                "\tns_med=%.3f\tspread_pct=%.1f%s\tchk=%08x",
                suite, name, extra ? "\t" : "", extra ? extra : "", unit, ops, reps, mn, md, mx, ns_med, spread, bw,
                (unsigned)chk);
    else
        bench_emit("suite=%s\tname=%s%s%s\tunit=%s\tops=%.0f\treps=%d\tns_min=%.3f\tns_med=%.3f\tns_max=%.3f"
                "\tspread_pct=%.1f%s\tchk=%08x",
                suite, name, extra ? "\t" : "", extra ? extra : "", unit, ops, reps, mn / e->ticks_per_ns, ns_med,
                mx / e->ticks_per_ns, spread, bw, (unsigned)chk);
}

static void fail(const char *suite, const char *what)
{
    bench_emit("suite=%s\tname=error\treason=%s", suite, what);
}

/* ---- cpu -------------------------------------------------------------------------------------------------------- */

NOINL static uint32_t k_empty(kc *c)
{
    const size_t n = c->n;
    uint32_t x = 0;
    for (size_t i = 0; i < n; i++) OPQ(x);
    return x;
}

/* one step = shift, xor, add, each dependent on the previous step */
NOINL static uint32_t k_alu(kc *c)
{
    const size_t n = c->n;
    uint32_t x = s_seed, k = s_seed | 1u;
    for (size_t i = 0; i < n; i++) {
        x = (x ^ (x << 3)) + k;
        OPQ(x);
        x = (x ^ (x >> 5)) + k;
        OPQ(x);
        x = (x ^ (x << 7)) + k;
        OPQ(x);
        x = (x ^ (x >> 2)) + k;
        OPQ(x);
    }
    return x;
}

/* the same steps in four independent chains (instruction-level parallelism) */
NOINL static uint32_t k_alu_indep(kc *c)
{
    const size_t n = c->n;
    uint32_t a = s_seed, b = a + 1, d = a + 2, f = a + 3, k = s_seed | 1u;
    for (size_t i = 0; i < n; i++) {
        a = (a ^ (a << 3)) + k;
        b = (b ^ (b >> 5)) + k;
        d = (d ^ (d << 7)) + k;
        f = (f ^ (f >> 2)) + k;
        OPQ(a);
        OPQ(b);
        OPQ(d);
        OPQ(f);
    }
    return a ^ b ^ d ^ f;
}

NOINL static uint32_t k_mul32(kc *c)
{
    const size_t n = c->n;
    uint32_t x = s_seed, k = s_seed | 1u;
    for (size_t i = 0; i < n; i++) {
        x = x * k + (uint32_t)i;
        OPQ(x);
        x = x * k + (uint32_t)i;
        OPQ(x);
        x = x * k + (uint32_t)i;
        OPQ(x);
        x = x * k + (uint32_t)i;
        OPQ(x);
    }
    return x;
}

NOINL static uint32_t k_div32(kc *c)
{
    const size_t n = c->n;
    uint32_t x = s_seed, k = s_seed | 0x80000000u;
    for (size_t i = 0; i < n; i++) {
        x = k / ((x & 0xFFFFu) | 1u) + (uint32_t)i;
        OPQ(x);
        x = k / ((x & 0xFFFFu) | 1u) + (uint32_t)i;
        OPQ(x);
    }
    return x;
}

NOINL static uint32_t k_mul64(kc *c)
{
    const size_t n = c->n;
    uint64_t y = (uint64_t)s_seed * 0x100000001ull, k = ((uint64_t)s_seed << 32) | 0x7F4A7C15u;
    for (size_t i = 0; i < n; i++) {
        y = y * k + i;
        OPQ(y);
        y = y * k + i;
        OPQ(y);
    }
    return (uint32_t)(y ^ (y >> 32));
}

NOINL static uint32_t k_div64(kc *c)
{
    const size_t n = c->n;
    uint64_t y = (uint64_t)s_seed * 0x100000001ull, k = ((uint64_t)s_seed << 32) | 0x80000000u;
    k |= 1ull << 63;
    for (size_t i = 0; i < n; i++) {
        y = k / ((y & 0xFFFFFFFFFFull) | 1u) + i;
        OPQ(y);
        y = k / ((y & 0xFFFFFFFFFFull) | 1u) + i;
        OPQ(y);
    }
    return (uint32_t)(y ^ (y >> 32));
}

/* one branch per byte; the asm in the taken arm keeps it a branch (no if-conversion) */
NOINL static uint32_t k_branch(kc *c)
{
    const uint8_t *a = c->src;
    const size_t len = c->n, passes = c->m;
    uint32_t s = 0;
    for (size_t r = 0; r < passes; r++)
        for (size_t i = 0; i < len; i++) {
            if (a[i] & 1u) {
                __asm__ volatile("");
                s += a[i];
            } else {
                s ^= (uint32_t)i;
            }
        }
    return s;
}

#define FLOAT_K(T, NAME, INIT, STEP)                                                                                 \
    NOINL static uint32_t NAME(kc *c)                                                                                \
    {                                                                                                                \
        const size_t n = c->n;                                                                                       \
        T a = (T)(sizeof(T) == 4 ? s_fa : s_da), b = (T)(sizeof(T) == 4 ? s_fb : s_db);                              \
        T x = (T)(INIT);                                                                                             \
        (void)a;                                                                                                     \
        (void)b;                                                                                                     \
        for (size_t i = 0; i < n; i++) {                                                                             \
            STEP;                                                                                                    \
            STEP;                                                                                                    \
            STEP;                                                                                                    \
            STEP;                                                                                                    \
        }                                                                                                            \
        return (uint32_t)(x * (T)1000);                                                                              \
    }
FLOAT_K(float, k_fadd, s_seed & 0xFFu, x = x + b)
FLOAT_K(float, k_fmul, s_seed & 0xFFu, x = x * a)
FLOAT_K(float, k_fmadd, s_seed & 0xFFu, x = x * a + b)
FLOAT_K(float, k_fdiv, 1, x = a / (x + a))
FLOAT_K(double, k_dadd, s_seed & 0xFFu, x = x + b)
FLOAT_K(double, k_dmul, s_seed & 0xFFu, x = x * a)
FLOAT_K(double, k_dmadd, s_seed & 0xFFu, x = x * a + b)
FLOAT_K(double, k_ddiv, 1, x = a / (x + a))

NOINL static uint32_t k_inline(kc *c)
{
    const size_t n = c->n;
    uint32_t x = s_seed;
    for (size_t i = 0; i < n; i++) {
        x = x + (uint32_t)i;
        OPQ(x);
    }
    return x;
}

NOINL static uint32_t f_leaf(uint32_t a, uint32_t b)
{
    __asm__ volatile("");
    return a + b;
}
static uint32_t (*volatile s_leaf)(uint32_t, uint32_t) = f_leaf;

NOINL static uint32_t k_call(kc *c)
{
    const size_t n = c->n;
    uint32_t x = s_seed;
    for (size_t i = 0; i < n; i++) x = f_leaf(x, (uint32_t)i);
    return x;
}

NOINL static uint32_t k_icall(kc *c)
{
    const size_t n = c->n;
    uint32_t (*f)(uint32_t, uint32_t) = s_leaf;
    uint32_t x = s_seed;
    for (size_t i = 0; i < n; i++) x = f(x, (uint32_t)i);
    return x;
}

/* not a tail call (the result is used after the call), so the depth is real */
NOINL static uint32_t f_rec(uint32_t d, uint32_t x)
{
    if (d == 0) return x + 1u;
    uint32_t r = f_rec(d - 1u, x ^ d);
    OPQ(r);
    return r + d;
}

NOINL static uint32_t k_rec(kc *c)
{
    const size_t n = c->n;
    const uint32_t depth = (uint32_t)c->m;
    uint32_t x = s_seed;
    for (size_t i = 0; i < n; i++) x = f_rec(depth, x);
    return x;
}

/* element / class names as a CSS styler sees them in an EPUB */
static const char *const k_names[32] = {
    "p",        "div",      "span",      "ruby",      "rt",      "rb",     "a",        "img",
    "h1",       "h2",       "section",   "body",      "html",    "br",     "em",       "strong",
    "calibre1", "calibre10", "calibre12", "calibre2", "main",    "chapter", "vrtl",    "hltr",
    "tcy",      "gfont",    "sesame",    "kenten",    "bold",    "italic", "pagebreak", "title"};

NOINL static uint32_t k_strcmp_matrix(kc *c)
{
    uint32_t eq = 0;
    for (size_t r = 0; r < c->n; r++) {
        BARRIER();
        for (int i = 0; i < 32; i++)
            for (int j = 0; j < 32; j++) eq += strcmp(c->names[i], c->names[j]) == 0;
    }
    return eq;
}

NOINL static uint32_t k_strcmp_equal(kc *c)
{
    uint32_t eq = 0;
    for (size_t r = 0; r < c->n; r++) {
        BARRIER();
        for (int i = 0; i < 32; i++) eq += strcmp(c->names[i], c->names2[i]) == 0;
    }
    return eq;
}

/* 32 records of 16 bytes in src and their copies in dst; m = compared length (8 or 16) */
NOINL static uint32_t k_memcmp(kc *c)
{
    uint32_t eq = 0;
    const size_t len = c->m;
    for (size_t r = 0; r < c->n; r++) {
        BARRIER();
        for (int i = 0; i < 32; i++) eq += memcmp(c->src + 16 * i, c->dst + 16 * i, len) == 0;
    }
    return eq;
}

NOINL static uint32_t k_strlen(kc *c)
{
    uint32_t s = 0;
    for (size_t r = 0; r < c->n; r++) {
        BARRIER();
        for (int i = 0; i < 32; i++) s += (uint32_t)strlen(c->names[i]);
    }
    return s;
}

NOINL static uint32_t k_fnv(kc *c)
{
    uint32_t s = 0;
    for (size_t r = 0; r < c->n; r++) {
        BARRIER();
        for (int i = 0; i < 32; i++) {
            uint32_t h = 2166136261u;
            for (const unsigned char *q = (const unsigned char *)c->names[i]; *q; q++) h = (h ^ *q) * 16777619u;
            s += h;
        }
    }
    return s;
}

static uint32_t s_utf8_chars;

NOINL static uint32_t k_utf8(kc *c)
{
    const uint8_t *p = c->src, *end = c->src + c->n;
    uint32_t acc = 0, n = 0;
    while (p < end) {
        uint32_t ch = *p;
        if (ch < 0x80u) {
            p++;
        } else if (ch < 0xE0u) {
            if (end - p < 2) break;
            ch = ((ch & 0x1Fu) << 6) | (p[1] & 0x3Fu);
            p += 2;
        } else if (ch < 0xF0u) {
            if (end - p < 3) break;
            ch = ((ch & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
            p += 3;
        } else {
            if (end - p < 4) break;
            ch = ((ch & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) | ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
            p += 4;
        }
        acc = acc * 31u + ch;
        n++;
    }
    s_utf8_chars = n;
    return acc;
}

static void put_utf8(uint8_t **q, uint32_t cp)
{
    uint8_t *p = *q;
    if (cp < 0x80u) {
        *p++ = (uint8_t)cp;
    } else if (cp < 0x800u) {
        *p++ = (uint8_t)(0xC0u | (cp >> 6));
        *p++ = (uint8_t)(0x80u | (cp & 0x3Fu));
    } else {
        *p++ = (uint8_t)(0xE0u | (cp >> 12));
        *p++ = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        *p++ = (uint8_t)(0x80u | (cp & 0x3Fu));
    }
    *q = p;
}

/* Japanese-looking text: 45 % kanji, 30 % kana, 10 % 、。「」, 15 % ASCII; exactly len bytes; returns the char count */
static uint32_t gen_text(uint8_t *buf, size_t len)
{
    uint32_t st = 0x1234567u, n = 0;
    uint8_t *p = buf, *end = buf + len;
    static const uint32_t punct[4] = {0x3001u, 0x3002u, 0x300Cu, 0x300Du};
    while (end - p >= 3) {
        const uint32_t r = xs32(&st) % 100u;
        uint32_t cp;
        if (r < 45u) cp = 0x4E00u + xs32(&st) % 0x5000u;
        else if (r < 75u) cp = 0x3041u + xs32(&st) % 0xB6u;
        else if (r < 85u) cp = punct[xs32(&st) & 3u];
        else cp = 0x20u + xs32(&st) % 0x5Fu;
        put_utf8(&p, cp);
        n++;
    }
    while (p < end) {
        *p++ = 'a';
        n++;
    }
    return n;
}

static int cpu_strings(const bench_env *e, uint32_t *t, int where)
{
    char ex[64];
    const char *mem = e->mem_name[where];
    /* both copies of the names in this memory class, NUL-terminated, packed as a CSS styler's string pool would be */
    char *pool = e->alloc(2 * 512, where);
    uint8_t *rec = e->alloc(2 * 512, where);
    if (!pool || !rec) {
        e->free(pool);
        e->free(rec);
        fail("cpu", "no_memory_strings");
        return 1;
    }
    const char *a[32], *b[32];
    char *q = pool;
    for (int k = 0; k < 2; k++)
        for (int i = 0; i < 32; i++) {
            const size_t l = strlen(k_names[i]) + 1;
            memcpy(q, k_names[i], l);
            (k ? b : a)[i] = q;
            q += l;
        }
    for (int i = 0; i < 32; i++) {
        memcpy(rec + 16 * i, "calibre-class-00", 16);
        rec[16 * i + 15] = (uint8_t)('A' + i);
        memcpy(rec + 512 + 16 * i, rec + 16 * i, 16);
    }
    kc c = {.e = e, .names = a, .names2 = b, .src = rec, .dst = rec + 512};
    int bad = 0;
    snprintf(ex, sizeof ex, "mem=%s", mem);
    c.n = 64;
    uint32_t v = run(e, 1, k_strcmp_matrix, &c, NULL, t);
    bench_report(e, "cpu", "strcmp_matrix", ex, 64.0 * 1024, "cmp", t, v);
    c.n = 2000;
    v = run(e, 1, k_strcmp_equal, &c, NULL, t);
    bench_report(e, "cpu", "strcmp_equal", ex, 2000.0 * 32, "cmp", t, v);
    c.m = 8;
    v = run(e, 1, k_memcmp, &c, NULL, t);
    bench_report(e, "cpu", "memcmp8_equal", ex, 2000.0 * 32, "cmp", t, v);
    c.m = 16;
    v = run(e, 1, k_memcmp, &c, NULL, t);
    bench_report(e, "cpu", "memcmp16_equal", ex, 2000.0 * 32, "cmp", t, v);
    v = run(e, 1, k_strlen, &c, NULL, t);
    bench_report(e, "cpu", "strlen", ex, 2000.0 * 32, "str", t, v);
    v = run(e, 1, k_fnv, &c, NULL, t);
    bench_report(e, "cpu", "fnv1a", ex, 2000.0 * 32, "str", t, v);
    /* self-check: every name equals only itself; all copies compare equal */
    c.n = 1;
    bad += k_strcmp_matrix(&c) != 32u;
    bad += k_strcmp_equal(&c) != 32u;
    c.m = 16;
    bad += k_memcmp(&c) != 32u;
    bench_emit("suite=cpu\tname=strings_check\tmem=%s\tok=%d", mem, bad == 0);
    e->free(pool);
    e->free(rec);
    return bad;
}

int bench_cpu(const bench_env *e)
{
    uint32_t t[MAXREPS];
    int bad = 0;
    kc c = {.e = e};
    uint32_t v;
    char ex[96];

    c.n = 200000;
    v = run(e, 1, k_empty, &c, NULL, t);
    bench_report(e, "cpu", "loop_empty", NULL, (double)c.n, "iter", t, v);
    c.n = 50000;
    v = run(e, 1, k_alu, &c, NULL, t);
    bench_report(e, "cpu", "alu_dep", "note=shift+xor+add", 4.0 * c.n, "step", t, v);
    v = run(e, 1, k_alu_indep, &c, NULL, t);
    bench_report(e, "cpu", "alu_indep4", "note=shift+xor+add", 4.0 * c.n, "step", t, v);
    v = run(e, 1, k_mul32, &c, NULL, t);
    bench_report(e, "cpu", "mul32_dep", "note=mul+add", 4.0 * c.n, "op", t, v);
    c.n = 20000;
    v = run(e, 1, k_div32, &c, NULL, t);
    bench_report(e, "cpu", "div32_dep", "note=udiv+and+or+add", 2.0 * c.n, "op", t, v);
    v = run(e, 1, k_mul64, &c, NULL, t);
    bench_report(e, "cpu", "mul64_dep", "note=mul+add", 2.0 * c.n, "op", t, v);
    c.n = 4000;
    v = run(e, 1, k_div64, &c, NULL, t);
    bench_report(e, "cpu", "div64_dep", "note=udiv64+and+or+add", 2.0 * c.n, "op", t, v);

    /* branches over 4 KB of internal bytes: random, always taken, alternating */
    uint8_t *br = e->alloc(4 * KB, BENCH_INT);
    if (br) {
        static const char *const pat[3] = {"random", "taken", "alternate"};
        for (int k = 0; k < 3; k++) {
            uint32_t st = 77u;
            for (size_t i = 0; i < 4 * KB; i++)
                br[i] = k == 0 ? (uint8_t)xs32(&st) : k == 1 ? (uint8_t)(2u * i + 1u) : (uint8_t)i;
            c.src = br;
            c.n = 4 * KB;
            c.m = 25;
            v = run(e, 1, k_branch, &c, NULL, t);
            snprintf(ex, sizeof ex, "pattern=%s", pat[k]);
            bench_report(e, "cpu", "branch", ex, 25.0 * 4 * KB, "byte", t, v);
        }
        e->free(br);
    } else {
        fail("cpu", "no_memory_branch");
        bad++;
    }

    static const struct {
        const char *name;
        kfn fn;
        size_t n;
    } fl[] = {{"float_add", k_fadd, 25000},  {"float_mul", k_fmul, 25000},  {"float_madd", k_fmadd, 25000},
              {"float_div", k_fdiv, 5000},   {"double_add", k_dadd, 5000},  {"double_mul", k_dmul, 5000},
              {"double_madd", k_dmadd, 5000}, {"double_div", k_ddiv, 1500}};
    for (size_t i = 0; i < sizeof fl / sizeof fl[0]; i++) {
        c.n = fl[i].n;
        v = run(e, 1, fl[i].fn, &c, NULL, t);
        bench_report(e, "cpu", fl[i].name, "note=dependent_chain", 4.0 * c.n, "op", t, v);
    }

    c.n = 100000;
    v = run(e, 1, k_inline, &c, NULL, t);
    bench_report(e, "cpu", "call_inline", "note=add_no_call", (double)c.n, "call", t, v);
    v = run(e, 1, k_call, &c, NULL, t);
    bench_report(e, "cpu", "call_direct", "note=leaf_2args", (double)c.n, "call", t, v);
    v = run(e, 1, k_icall, &c, NULL, t);
    bench_report(e, "cpu", "call_indirect", "note=leaf_2args_fnptr", (double)c.n, "call", t, v);
    static const uint32_t depth[] = {1, 4, 8, 12, 16, 32};
    for (size_t i = 0; i < sizeof depth / sizeof depth[0]; i++) {
        c.m = depth[i];
        c.n = 40000 / (depth[i] + 1);
        v = run(e, 1, k_rec, &c, NULL, t);
        snprintf(ex, sizeof ex, "depth=%u", (unsigned)depth[i]);
        bench_report(e, "cpu", "call_recursive", ex, (double)c.n * (depth[i] + 1), "call", t, v);
    }

    bad += cpu_strings(e, t, BENCH_INT);
    bad += cpu_strings(e, t, BENCH_EXT);

    /* UTF-8 decode: 8 KB internal, 8 KB PSRAM (cache-resident after the first repetition), 256 KB PSRAM (streams) */
    static const struct {
        int where;
        size_t len;
        const char *label;
    } ut[] = {{BENCH_INT, 8 * KB, "hot"}, {BENCH_EXT, 8 * KB, "hot"}, {BENCH_EXT, 256 * KB, "stream"}};
    for (size_t i = 0; i < sizeof ut / sizeof ut[0]; i++) {
        uint8_t *buf = e->alloc(ut[i].len, ut[i].where);
        if (!buf) {
            fail("cpu", "no_memory_utf8");
            bad++;
            continue;
        }
        const uint32_t want = gen_text(buf, ut[i].len);
        c.src = buf;
        c.n = ut[i].len;
        v = run(e, 1, k_utf8, &c, NULL, t);
        const int ok = s_utf8_chars == want;
        bad += !ok;
        snprintf(ex, sizeof ex, "mem=%s\tbytes=%u\tset=%s\tchars=%u\tbytes_per_char=%.3f\tok=%d", e->mem_name[ut[i].where],
                 (unsigned)ut[i].len, ut[i].label, (unsigned)want, (double)ut[i].len / want, ok);
        bench_report(e, "cpu", "utf8_decode", ex, (double)ut[i].len, "B", t, v);
        e->free(buf);
    }
    return bad;
}

/* ---- memory ----------------------------------------------------------------------------------------------------- */

NOINL static uint32_t k_read(kc *c)
{
    uint32_t s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    const size_t words = c->n / 4;
    for (size_t pass = 0; pass < c->m; pass++) {
        const uint32_t *p = (const uint32_t *)c->src;
        OPQ(p);
        for (size_t i = 0; i + 4 <= words; i += 4) {
            s0 += p[i];
            s1 ^= p[i + 1];
            s2 += p[i + 2];
            s3 ^= p[i + 3];
        }
    }
    return s0 + s1 + s2 + s3;
}

NOINL static uint32_t k_write(kc *c)
{
    const size_t words = c->n / 4;
    uint32_t v = s_seed;
    for (size_t pass = 0; pass < c->m; pass++) {
        uint32_t *p = (uint32_t *)c->dst;
        OPQ(p);
        for (size_t i = 0; i + 4 <= words; i += 4) {
            p[i] = v;
            p[i + 1] = v;
            p[i + 2] = v;
            p[i + 3] = v;
        }
        BARRIER();
    }
    return ((const uint32_t *)c->dst)[0];
}

NOINL static uint32_t k_memset(kc *c)
{
    for (size_t pass = 0; pass < c->m; pass++) {
        memset(c->dst, (int)(pass & 0x7Fu), c->n);
        BARRIER();
    }
    return c->dst[0];
}

/* memcpy of n bytes in chunks of m from src to dst; the offsets wrap at the source size q and the destination size p */
NOINL static uint32_t k_memcpy(kc *c)
{
    const size_t chunk = c->m, dsz = (size_t)(uintptr_t)c->p, ssz = c->q;
    size_t d = 0, s = 0;
    for (size_t done = 0; done + chunk <= c->n; done += chunk) {
        memcpy(c->dst + d, c->src + s, chunk);
        BARRIER();
        d += chunk;
        if (d + chunk > dsz) d = 0;
        s += chunk;
        if (s + chunk > ssz) s = 0;
    }
    return c->dst[0];
}

static uint8_t *s_evict;
static size_t s_evict_size;

/* reads 64 KB of PSRAM (twice the data cache) so the next repetition starts cold */
static void evict(kc *c)
{
    (void)c;
    if (!s_evict) return;
    uint32_t s = 0;
    for (size_t i = 0; i < s_evict_size; i += 16) s += s_evict[i];
    bench_sink(s);
}

static size_t int_size(const bench_env *e)
{
    size_t s = 32 * KB;
    while (s > 4 * KB && 2 * s + 8 * KB > e->int_max) s /= 2;
    return s;
}

int bench_mem(const bench_env *e)
{
    uint32_t t[MAXREPS];
    char ex[128];
    const size_t si = int_size(e), sp = 1 * MB;
    uint8_t *a = e->alloc(si, BENCH_INT), *b = e->alloc(si, BENCH_INT);
    uint8_t *p1 = e->alloc(sp, BENCH_EXT), *p2 = e->alloc(sp, BENCH_EXT);
    s_evict_size = 64 * KB;
    s_evict = e->alloc(s_evict_size, BENCH_EXT);
    int bad = 0;
    if (!a || !b || !p1 || !p2 || !s_evict) {
        fail("mem", "no_memory");
        bad = 1;
        goto out;
    }
    memset(a, 1, si);
    memset(b, 2, si);
    memset(p1, 3, sp);
    memset(p2, 4, sp);
    memset(s_evict, 5, s_evict_size);

    /* classes for read / write / memset: buffer, bytes per pass, passes (≈ 1 MB per repetition) */
    struct cls {
        const char *mem;
        uint8_t *buf;
        size_t bytes, passes;
        int writable;
    } cl[4] = {
        {e->mem_name[BENCH_INT], a, si, (1 * MB) / si, 1},
        {"psram_hot8k", p1, 8 * KB, 128, 1},
        {e->mem_name[BENCH_EXT], p1, sp, 1, 1},
        {bench_ro_name, (uint8_t *)(uintptr_t)bench_ro_region, bench_ro_size, 1, 0},
    };
    for (int k = 0; k < 4; k++) {
        if (!cl[k].buf || !cl[k].bytes) continue;
        kc c = {.e = e, .src = cl[k].buf, .dst = cl[k].buf, .n = cl[k].bytes, .m = cl[k].passes};
        const double bytes = (double)cl[k].bytes * (double)cl[k].passes;
        snprintf(ex, sizeof ex, "mem=%s\tset=%u", cl[k].mem, (unsigned)cl[k].bytes);
        uint32_t v = run(e, 1, k_read, &c, NULL, t);
        bench_report(e, "mem", "read", ex, bytes, "B", t, v);
        if (!cl[k].writable) continue;
        v = run(e, 1, k_write, &c, NULL, t);
        bench_report(e, "mem", "write", ex, bytes, "B", t, v);
        v = run(e, 1, k_memset, &c, NULL, t);
        bench_report(e, "mem", "memset", ex, bytes, "B", t, v);
    }

    /* memcpy: within and across classes; chunk = the internal buffer size (or 4 KB for the hot PSRAM pair) */
    /* total bytes, chunk, source size (wraps: a small source is re-read hot), destination size */
    struct cp {
        const char *name;
        const uint8_t *src;
        uint8_t *dst;
        size_t total, chunk, ssz, dsz;
    } cps[] = {
        {"int_to_int", a, b, 1 * MB, si, si, si},
        {"psram_hot4k_to_psram_hot4k", p1, p1 + 4 * KB, 1 * MB, 4 * KB, 4 * KB, 4 * KB},
        {"psram_to_psram", p1, p2, sp, si, sp, sp},
        {"psram_to_int", p1, a, sp, si, sp, si},
        {"int_to_psram", a, p2, sp, si, si, sp},
        {"rodata_to_int", bench_ro_region, a, bench_ro_size, si, bench_ro_size, si},
    };
    for (size_t k = 0; k < sizeof cps / sizeof cps[0]; k++) {
        if (!cps[k].src || !cps[k].total) continue;
        kc c = {.e = e, .src = cps[k].src, .dst = cps[k].dst, .n = cps[k].total, .m = cps[k].chunk, .q = cps[k].ssz,
                .p = (void *)(uintptr_t)cps[k].dsz};
        uint32_t v = run(e, 1, k_memcpy, &c, NULL, t);
        snprintf(ex, sizeof ex, "dir=%s\tchunk=%u", cps[k].name, (unsigned)cps[k].chunk);
        bench_report(e, "mem", "memcpy", ex, (double)(cps[k].total / cps[k].chunk * cps[k].chunk), "B", t, v);
    }
out:
    e->free(a);
    e->free(b);
    e->free(p1);
    e->free(p2);
    e->free(s_evict);
    s_evict = NULL;
    return bad;
}

/* ---- pointer chase ---------------------------------------------------------------------------------------------- */

#define LINE 32u /* one node per data-cache line (CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE) */
#define AUXW 4   /* the node's 32-bit word at byte 16: the permutation while building, the rmw counter after */

NOINL static uint32_t k_chase(kc *c)
{
    void *p = c->p;
    size_t h = c->n;
    while (h >= 8) {
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        p = *(void **)p;
        h -= 8;
    }
    while (h--) p = *(void **)p;
    c->p = p;
    return (uint32_t)(uintptr_t)p;
}

/* the same walk, each node also written (the line becomes dirty and is written back on eviction) */
NOINL static uint32_t k_chase_rmw(kc *c)
{
    void *p = c->p;
    for (size_t h = c->n; h; h--) {
        void **q = p;
        ((uint32_t *)q)[AUXW] += 1u;
        p = *q;
    }
    c->p = p;
    return (uint32_t)(uintptr_t)p;
}

/* One random cycle (Sattolo) through every LINE-byte node of buf[0 .. ws): node i's first word points to the next.
 * The permutation is kept in each node's word AUXW while it is built. seq: nodes in address order instead. */
static void build_chain(uint8_t *buf, size_t ws, int seq, uint32_t seed)
{
    const size_t nodes = ws / LINE;
    for (size_t i = 0; i < nodes; i++) ((uint32_t *)(buf + i * LINE))[AUXW] = (uint32_t)i;
    if (!seq)
        for (size_t i = nodes - 1; i > 0; i--) {
            const size_t j = xs32(&seed) % i;
            uint32_t *a = (uint32_t *)(buf + i * LINE) + AUXW, *b = (uint32_t *)(buf + j * LINE) + AUXW;
            const uint32_t t = *a;
            *a = *b;
            *b = t;
        }
    for (size_t k = 0; k < nodes; k++) {
        const uint32_t from = ((uint32_t *)(buf + k * LINE))[AUXW];
        const uint32_t to = ((uint32_t *)(buf + ((k + 1) % nodes) * LINE))[AUXW];
        *(void **)(buf + (size_t)from * LINE) = buf + (size_t)to * LINE;
    }
}

static int chase_one(const bench_env *e, uint8_t *buf, size_t ws, int where, int seq, int rmw, uint32_t *t)
{
    char ex[128];
    build_chain(buf, ws, seq, 0xC0FFEEu + (uint32_t)ws);
    const size_t nodes = ws / LINE;
    kc c = {.e = e, .p = buf, .n = nodes};
    k_chase(&c); /* warm: one full lap, and the self-check: a lap ends where it started */
    const int ok = c.p == buf;
    c.n = nodes < 4096 ? 32768 : nodes < 32768 ? nodes * 2 : 32768;
    const uint32_t v = run(e, 1, rmw ? k_chase_rmw : k_chase, &c, NULL, t);
    snprintf(ex, sizeof ex, "mem=%s\tws=%u\tnodes=%u\torder=%s\tok=%d", e->mem_name[where], (unsigned)ws,
             (unsigned)nodes, seq ? "seq" : "random", ok);
    bench_report(e, "chase", rmw ? "chase_rmw" : "chase", ex, (double)c.n, "hop", t, v);
    return !ok;
}

int bench_chase(const bench_env *e)
{
    uint32_t t[MAXREPS];
    int bad = 0;
    static const size_t ws_ext[] = {1 * KB,   2 * KB,   4 * KB,   8 * KB,   16 * KB, 24 * KB, 32 * KB, 48 * KB,
                                    64 * KB,  128 * KB, 256 * KB, 512 * KB, 1 * MB,  2 * MB,  4 * MB};
    size_t imax = 128 * KB;
    while (imax > 1 * KB && imax + 8 * KB > e->int_max) imax /= 2;
    uint8_t *ib = e->alloc(imax, BENCH_INT);
    if (ib) {
        for (size_t ws = 1 * KB; ws <= imax; ws *= 2) bad += chase_one(e, ib, ws, BENCH_INT, 0, 0, t);
        e->free(ib);
    } else {
        fail("chase", "no_memory_int");
        bad++;
    }
    uint8_t *pb = e->alloc(4 * MB, BENCH_EXT);
    if (!pb) {
        fail("chase", "no_memory_psram");
        return bad + 1;
    }
    for (size_t i = 0; i < sizeof ws_ext / sizeof ws_ext[0]; i++) bad += chase_one(e, pb, ws_ext[i], BENCH_EXT, 0, 0, t);
    static const size_t ws_rmw[] = {8 * KB, 32 * KB, 64 * KB, 1 * MB, 4 * MB};
    for (size_t i = 0; i < sizeof ws_rmw / sizeof ws_rmw[0]; i++) bad += chase_one(e, pb, ws_rmw[i], BENCH_EXT, 0, 1, t);
    bad += chase_one(e, pb, 1 * MB, BENCH_EXT, 1, 0, t); /* address order: what sequential line fills cost */
    e->free(pb);
    return bad;
}

/* ---- stride ----------------------------------------------------------------------------------------------------- */

NOINL static uint32_t k_stride(kc *c)
{
    const uint8_t *p = c->src;
    const size_t size = c->n, s = c->m;
    uint32_t acc = 0;
    for (size_t i = 0; i < size; i += s) acc += p[i];
    return acc;
}

int bench_stride(const bench_env *e)
{
    uint32_t t[MAXREPS];
    char ex[96];
    const size_t si = int_size(e);
    uint8_t *a = e->alloc(si, BENCH_INT), *p = e->alloc(1 * MB, BENCH_EXT);
    s_evict_size = 64 * KB;
    s_evict = e->alloc(s_evict_size, BENCH_EXT);
    int bad = 0;
    if (!a || !p || !s_evict) {
        fail("stride", "no_memory");
        bad = 1;
        goto out;
    }
    memset(a, 1, si);
    memset(p, 2, 1 * MB);
    memset(s_evict, 3, s_evict_size);
    const struct {
        const char *mem;
        const uint8_t *buf;
        size_t size;
    } cl[3] = {{e->mem_name[BENCH_INT], a, si}, {e->mem_name[BENCH_EXT], p, 1 * MB}, {bench_ro_name, bench_ro_region, bench_ro_size}};
    for (int k = 0; k < 3; k++) {
        if (!cl[k].buf || !cl[k].size) continue;
        for (size_t s = 4; s <= 512; s *= 2) {
            kc c = {.e = e, .src = cl[k].buf, .n = cl[k].size, .m = s};
            const uint32_t v = run(e, 1, k_stride, &c, evict, t);
            snprintf(ex, sizeof ex, "mem=%s\tset=%u\tstride=%u", cl[k].mem, (unsigned)cl[k].size, (unsigned)s);
            bench_report(e, "stride", "load", ex, (double)(cl[k].size / s), "load", t, v);
        }
    }
out:
    e->free(a);
    e->free(p);
    e->free(s_evict);
    s_evict = NULL;
    return bad;
}

/* ---- struct copy ------------------------------------------------------------------------------------------------ */

#define STRUCT_K(S)                                                                                                  \
    typedef struct {                                                                                                 \
        uint32_t w[(S) / 4];                                                                                         \
    } st##S;                                                                                                         \
    NOINL static uint32_t k_st##S(kc *c)                                                                             \
    {                                                                                                                \
        const st##S *s = (const st##S *)c->src;                                                                      \
        st##S *d = (st##S *)c->dst;                                                                                  \
        for (size_t pass = 0; pass < c->m; pass++)                                                                   \
            for (size_t i = 0; i < c->n; i++) {                                                                      \
                d[i] = s[i];                                                                                         \
                BARRIER();                                                                                           \
            }                                                                                                        \
        return d[c->n - 1].w[0];                                                                                     \
    }
STRUCT_K(64)
STRUCT_K(256)
STRUCT_K(1024)

int bench_struct(const bench_env *e)
{
    uint32_t t[MAXREPS];
    char ex[96];
    int bad = 0;
    uint8_t *ia = e->alloc(8 * KB, BENCH_INT);
    uint8_t *pa = e->alloc(8 * KB, BENCH_EXT);
    uint8_t *pc = e->alloc(512 * KB, BENCH_EXT);
    if (!ia || !pa || !pc) {
        fail("struct", "no_memory");
        bad = 1;
        goto out;
    }
    memset(ia, 1, 8 * KB);
    memset(pa, 2, 8 * KB);
    memset(pc, 3, 512 * KB);
    static const struct {
        size_t size;
        kfn fn;
    } ks[3] = {{64, k_st64}, {256, k_st256}, {1024, k_st1024}};
    const struct {
        const char *mem;
        uint8_t *buf;
        size_t half, passes;
    } cl[3] = {{e->mem_name[BENCH_INT], ia, 4 * KB, 16}, {"psram_hot", pa, 4 * KB, 16}, {"psram_cold", pc, 256 * KB, 1}};
    for (int k = 0; k < 3; k++)
        for (int s = 0; s < 3; s++) {
            kc c = {.e = e, .src = cl[k].buf, .dst = cl[k].buf + cl[k].half, .n = cl[k].half / ks[s].size,
                    .m = cl[k].passes};
            const uint32_t v = run(e, 1, ks[s].fn, &c, NULL, t);
            snprintf(ex, sizeof ex, "mem=%s\tsize=%u\tset=%u", cl[k].mem, (unsigned)ks[s].size, (unsigned)(2 * cl[k].half));
            bench_report(e, "struct", "copy", ex, (double)c.n * c.m, "copy", t, v);
        }
out:
    e->free(ia);
    e->free(pa);
    e->free(pc);
    return bad;
}

/* ---- allocator -------------------------------------------------------------------------------------------------- */

enum { CLS_INT = BENCH_INT, CLS_EXT = BENCH_EXT, CLS_LIBC = 2 };
static const char *const k_cls_name[3] = {"caps_int", "caps_psram", "libc_malloc"};

static void *cls_alloc(const bench_env *e, int cls, size_t n)
{
    return cls == CLS_LIBC ? malloc(n) : e->malloc_cls(n, cls);
}
static void cls_free(const bench_env *e, int cls, void *p)
{
    if (cls == CLS_LIBC) free(p);
    else e->free_cls(p);
}

static uint32_t s_alloc_fails;

NOINL static uint32_t k_pair(kc *c)
{
    const int cls = (int)c->m;
    const size_t size = (size_t)(uintptr_t)c->p;
    uint32_t s = 0;
    for (size_t i = 0; i < c->n; i++) {
        uint8_t *q = cls_alloc(c->e, cls, size);
        if (!q) {
            s_alloc_fails++;
            continue;
        }
        q[0] = (uint8_t)i;
        s += q[0];
        cls_free(c->e, cls, q);
    }
    return s;
}

#define BATCH 256

static void alloc_batch(const bench_env *e, int cls, size_t size)
{
    void *v[BATCH];
    uint32_t ta[MAXREPS], tl[MAXREPS], tf[MAXREPS];
    char ex[96];
    for (int r = 0; r < reps_of(e); r++)
        for (int order = 0; order < 2; order++) {
            uint32_t t0 = e->ticks();
            for (int i = 0; i < BATCH; i++) {
                v[i] = cls_alloc(e, cls, size);
                if (!v[i]) s_alloc_fails++;
            }
            const uint32_t t1 = e->ticks();
            if (order == 0)
                for (int i = BATCH - 1; i >= 0; i--) cls_free(e, cls, v[i]);
            else
                for (int i = 0; i < BATCH; i++) cls_free(e, cls, v[i]);
            const uint32_t t2 = e->ticks();
            if (order == 0) {
                ta[r] = t1 - t0;
                tl[r] = t2 - t1;
            } else {
                tf[r] = t2 - t1;
            }
            if (e->pause) e->pause();
        }
    snprintf(ex, sizeof ex, "cls=%s\tsize=%u\tbatch=%d", k_cls_name[cls], (unsigned)size, BATCH);
    bench_report(e, "alloc", "batch_malloc", ex, BATCH, "call", ta, 0);
    bench_report(e, "alloc", "batch_free_lifo", ex, BATCH, "call", tl, 0);
    bench_report(e, "alloc", "batch_free_fifo", ex, BATCH, "call", tf, 0);
}

/* N blocks of random sizes 16 … 512, every other one freed; then a malloc / free pair of 32 B and one of 2 KB in the
 * fragmented heap, and the free / largest figures before, holed and after (after = all freed: equals before) */
static int alloc_frag(const bench_env *e, int cls, size_t nblocks)
{
    uint32_t t[MAXREPS];
    char ex[192];
    void **v = malloc(sizeof(void *) * nblocks);
    if (!v) return 1;
    size_t f0 = 0, l0 = 0, f1 = 0, l1 = 0, f2 = 0, l2 = 0;
    if (e->heap_info) e->heap_info(cls, &f0, &l0);
    uint32_t st = 99u;
    for (size_t i = 0; i < nblocks; i++) v[i] = cls_alloc(e, cls, 16u + xs32(&st) % 497u);
    for (size_t i = 1; i < nblocks; i += 2) {
        cls_free(e, cls, v[i]);
        v[i] = NULL;
    }
    if (e->heap_info) e->heap_info(cls, &f1, &l1);
    kc c = {.e = e, .m = (size_t)cls, .n = 200, .p = (void *)(uintptr_t)32};
    uint32_t x = run(e, 0, k_pair, &c, NULL, t);
    snprintf(ex, sizeof ex, "cls=%s\tsize=32\tblocks=%u\tstate=holed", k_cls_name[cls], (unsigned)nblocks);
    bench_report(e, "alloc", "pair", ex, 200, "pair", t, x);
    c.p = (void *)(uintptr_t)2048;
    x = run(e, 0, k_pair, &c, NULL, t);
    snprintf(ex, sizeof ex, "cls=%s\tsize=2048\tblocks=%u\tstate=holed", k_cls_name[cls], (unsigned)nblocks);
    bench_report(e, "alloc", "pair", ex, 200, "pair", t, x);
    for (size_t i = 0; i < nblocks; i += 2) cls_free(e, cls, v[i]);
    if (e->heap_info) e->heap_info(cls, &f2, &l2);
    free(v);
    bench_emit("suite=alloc\tname=frag\tcls=%s\tblocks=%u\tfree_before=%u\tlargest_before=%u\tfree_holed=%u"
            "\tlargest_holed=%u\tfree_after=%u\tlargest_after=%u\tok=%d",
            k_cls_name[cls], (unsigned)nblocks, (unsigned)f0, (unsigned)l0, (unsigned)f1, (unsigned)l1, (unsigned)f2,
            (unsigned)l2, f2 == f0);
    return 0;
}

int bench_alloc(const bench_env *e)
{
    uint32_t t[MAXREPS];
    char ex[96];
    static const size_t sizes[] = {16, 64, 256, 1024, 4096};
    s_alloc_fails = 0;
    for (int cls = 0; cls < 3; cls++)
        for (size_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
            kc c = {.e = e, .m = (size_t)cls, .n = 1000, .p = (void *)(uintptr_t)sizes[i]};
            const uint32_t v = run(e, 0, k_pair, &c, NULL, t);
            snprintf(ex, sizeof ex, "cls=%s\tsize=%u\tstate=fresh", k_cls_name[cls], (unsigned)sizes[i]);
            bench_report(e, "alloc", "pair", ex, 1000, "pair", t, v);
        }
    for (int cls = 0; cls < 3; cls++) {
        alloc_batch(e, cls, 64);
        alloc_batch(e, cls, 1024);
    }
    size_t nint = e->int_max / 2 / 300;
    if (nint > 512) nint = 512;
    alloc_frag(e, CLS_INT, nint);
    alloc_frag(e, CLS_EXT, 2048);
    bench_emit("suite=alloc\tname=fails\tcount=%u\tok=%d", (unsigned)s_alloc_fails, s_alloc_fails == 0);
    return s_alloc_fails != 0;
}
