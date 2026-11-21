/*
 * bench core (portable: no ESP-IDF headers): the CPU, memory, pointer-chase, stride, struct-copy and allocator
 * micro-benchmarks of the `bench` console command (bench_main.c on the device). The same file builds on a host with a
 * small driver (../host/host_bench.c), so device / host ratios come from identical kernels. Results are "@BENCH" lines
 * through bench_emit.
 *
 * Timing: every measurement runs `reps` times; each repetition is timed alone with env->ticks (CPU cycles on the
 * device, nanoseconds on the host) inside env->quiet_begin / quiet_end (interrupts off on the device), and the line
 * carries min / median / max per operation and the spread (max − min) / min. Each kernel returns a value that is
 * folded into a checksum, so the optimiser cannot drop the work; the checksum is printed (chk=).
 */
#ifndef BENCH_CORE_H
#define BENCH_CORE_H

#include <stddef.h>
#include <stdint.h>

enum { BENCH_INT = 0, BENCH_EXT = 1 }; /* internal SRAM / PSRAM (the host: both plain heap) */

typedef struct {
    uint32_t (*ticks)(void);   /* free-running 32-bit counter; never let a timed region reach its wrap */
    double ticks_per_ns;       /* 0.24 at 240 MHz on the device; 1.0 on the host (ticks are ns) */
    int ticks_are_cycles;      /* the device: lines carry cyc_* fields; the host: ns_* only */
    void (*quiet_begin)(void); /* around each timed repetition (≤ ~50 ms); may be NULL */
    void (*quiet_end)(void);
    void (*pause)(void);       /* between measurements, outside timing (the device: let the idle tasks run) */
    void *(*alloc)(size_t n, int where); /* 32-byte aligned; NULL on failure */
    void (*free)(void *p);
    /* the allocator suite's subjects: a plain (unaligned) allocation of that class and its free; the device:
     * heap_caps_malloc with MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT / MALLOC_CAP_SPIRAM */
    void *(*malloc_cls)(size_t n, int where);
    void (*free_cls)(void *p);
    /* free bytes and largest free block of a class; NULL where unknown (the host) */
    void (*heap_info)(int where, size_t *free_bytes, size_t *largest);
    size_t int_max;           /* the largest internal block the memory suites may take */
    int reps;                  /* repetitions per measurement (≥ 3) */
    const char *mem_name[2];   /* "int" / "psram" (the host driver uses the same names) */
} bench_env;

/* Platform: prints one result line "@BENCH\t<fields>" (fields: tab-separated key=value pairs). Never called inside a
 * timed region. */
void bench_emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* One "@BENCH" line from ticks[0 .. reps-1] of `ops` operations each. extra: tab-separated key=value fields (may be
 * NULL). Also used by the ESP-only suites of bench_main.c. */
void bench_report(const bench_env *e, const char *suite, const char *name, const char *extra, double ops,
                  const char *unit, uint32_t *ticks, uint32_t chk);
/* Folds v into the run's checksum (kept in a volatile global). */
void bench_sink(uint32_t v);

/* The suites; each returns the number of lines that reported a failure (allocation, self-check). */
int bench_cpu(const bench_env *e);    /* ALU, mul / div 32 + 64, branches, float / double, calls, strings, UTF-8 */
int bench_mem(const bench_env *e);    /* sequential read / write / memcpy / memset, cross-class memcpy */
int bench_chase(const bench_env *e);  /* random pointer chasing 1 KB … 4 MB (read and read-modify-write) */
int bench_stride(const bench_env *e); /* one load every s bytes, s = 4 … 512 */
int bench_struct(const bench_env *e); /* struct assignment of 64 / 256 / 1024 B, hot and cold */
int bench_alloc(const bench_env *e);  /* malloc_cls / free_cls per class and libc malloc / free: pairs, batches,
                                         a fragmented heap */

/* A read-only region the memory suites read as a third class (the device: flash .rodata through the data cache);
 * set by the platform before bench_mem / bench_stride, NULL = skip. */
extern const uint8_t *bench_ro_region;
extern size_t bench_ro_size;
extern const char *bench_ro_name;

#endif
