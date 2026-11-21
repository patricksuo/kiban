/* Host driver for ../main/bench_core.c: the same portable kernels on the build machine (ticks = ns).
 * Build and run from examples/bench:
 *   cc -O2 -Imain host/host_bench.c main/bench_core.c -o host_bench
 *   ./host_bench [all|cpu|mem|chase|stride|struct|alloc] [reps=N] > host.log
 * "int" and "psram" are both the plain heap here. Uses clock_gettime(CLOCK_MONOTONIC); on macOS
 * clock_gettime_nsec_np(CLOCK_UPTIME_RAW) is used instead. */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>

#include "bench_core.h"

void bench_emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("@BENCH\t");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static uint32_t ticks(void)
{
#ifdef __APPLE__
    return (uint32_t)clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec);
#endif
}

static void *al(size_t n, int where)
{
    (void)where;
    void *p = NULL;
    return posix_memalign(&p, 32, n) ? NULL : p;
}

static void *mal(size_t n, int where)
{
    (void)where;
    return malloc(n);
}

static const uint8_t ro[128 * 1024] = {1};

int main(int argc, char **argv)
{
    const char *which = "all";
    int reps = 7;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "reps=", 5)) reps = atoi(argv[i] + 5);
        else which = argv[i];
    }
    bench_env e = {.ticks = ticks, .ticks_per_ns = 1.0, .ticks_are_cycles = 0, .alloc = al, .free = free,
                   .malloc_cls = mal, .free_cls = free, .int_max = 512 * 1024, .reps = reps,
                   .mem_name = {"int", "psram"}};
    bench_ro_region = ro;
    bench_ro_size = sizeof ro;
    struct utsname u;
    uname(&u);
    printf("@BENCH\tphase=env\thost=1\tsys=%s\tmachine=%s\trelease=%s\treps=%d\tnote=mem_classes_are_plain_heap\n",
           u.sysname, u.machine, u.release, reps);
    int bad = 0;
    const int all = !strcmp(which, "all");
    if (all || !strcmp(which, "cpu")) bad += bench_cpu(&e);
    if (all || !strcmp(which, "mem")) bad += bench_mem(&e);
    if (all || !strcmp(which, "chase")) bad += bench_chase(&e);
    if (all || !strcmp(which, "stride")) bad += bench_stride(&e);
    if (all || !strcmp(which, "struct")) bad += bench_struct(&e);
    if (all || !strcmp(which, "alloc")) bad += bench_alloc(&e);
    printf("@BENCH\tphase=done\tbad=%d\n", bad);
    return bad != 0;
}
