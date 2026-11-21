/*
 * kiban bench firmware: the device performance baseline (docs/research/004) as a serial console app.
 *
 *   mount [khz]                     remount the SD card (boot: 20 MHz; `mount 40000` for the 40 MHz bus)
 *   bench [suite ...] [reps=N] [file=<path>]
 *
 * Suites: cpu mem chase stride struct alloc (the portable kernels of bench_core.c), code (flash vs IRAM, I-cache
 * thrash) and sd (card I/O through the VFS); `all` (the default) runs them in that order. Each suite runs in its own
 * task pinned to core 1 with its result lines deferred (printed after the task ends, so console output never falls
 * inside a measurement); each timed repetition of the CPU / memory / code kernels runs with interrupts masked on that
 * core. Every result line is "@BENCH\t<key=value ...>"; each suite starts with a phase=env line.
 *
 * Card safety: the sd suite writes only below BENCH_DIR and deletes everything it wrote before it returns, also on
 * errors. Peak card use ≈ 4.2 MB (a 4 MB read file, a 128 KB fsync file) plus a directory of 128 empty files; it
 * refuses to start with less than 32 MB free. The optional file=<path> (any existing file > 64 KB, e.g. a large book)
 * is only opened O_RDONLY for random reads and open timings; its path is not printed. The card is never formatted.
 *
 * Board safety as the probe: no NVS / Wi-Fi / BLE, no charger writes beyond kiban_board_init()'s policy, amplifier off.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bench_core.h"
#include "esp_attr.h"
#include "esp_console.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "esp_ota_ops.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "kiban/board.h"
#include "kiban/sdcard.h"
#include "kiban_usb/usb_drive.h"
#include "sdkconfig.h"
#include "soc/soc.h"
#include "soc/spi_mem_reg.h"

static const char *TAG = "bench";

#define MNT "/sd"
#define BENCH_DIR MNT "/.kiban_bench" /* the sd suite's scratch directory; nothing else on the card is written */
#define BENCH_STACK 16384u
#define BENCH_CORE 1

#define NOINL __attribute__((noinline))
#define OPQ(x) __asm__ volatile("" : "+r"(x))

/* ---- result lines: printed directly, or deferred while a suite task runs ------------------------------------------ */

#define DEFER_CAP 32768
static SemaphoreHandle_t s_emit_lock;
static char s_line[1536];
static char *s_defer;
static size_t s_defer_len;
static int s_defer_on, s_defer_lost;

static void output(const char *line)
{
    printf("@BENCH\t%s\n", line);
    fflush(stdout);
}

void bench_emit(const char *fmt, ...)
{
    if (!s_emit_lock) s_emit_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_emit_lock, portMAX_DELAY);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_line, sizeof s_line, fmt, ap);
    va_end(ap);
    if (s_defer_on && s_defer) {
        const size_t need = strlen(s_line) + 1;
        if (s_defer_len + need < DEFER_CAP) {
            memcpy(s_defer + s_defer_len, s_line, need);
            s_defer_len += need;
        } else {
            s_defer_lost++;
        }
    } else {
        output(s_line);
    }
    xSemaphoreGive(s_emit_lock);
}

static void defer_begin(void)
{
    if (!s_defer) s_defer = heap_caps_malloc(DEFER_CAP, MALLOC_CAP_SPIRAM);
    s_defer_len = 0;
    s_defer_lost = 0;
    s_defer_on = 1;
}

static void defer_flush(void)
{
    s_defer_on = 0;
    for (size_t i = 0; s_defer && i < s_defer_len;) {
        const char *rec = s_defer + i;
        output(rec);
        i += strlen(rec) + 1;
    }
    s_defer_len = 0;
    if (s_defer_lost) printf("(%d result lines lost: defer buffer full)\n", s_defer_lost);
}

/* Runs fn(arg) in a task pinned to core with deferred output and waits for it. */
typedef struct {
    void (*fn)(void *);
    void *arg;
    SemaphoreHandle_t done;
} task_run;

static void task_tramp(void *p)
{
    task_run *r = p;
    r->fn(r->arg);
    xSemaphoreGive(r->done);
    vTaskDelete(NULL);
}

static void run_task_core(const char *name, uint32_t stack, void (*fn)(void *), void *arg, int core)
{
    task_run r = {.fn = fn, .arg = arg, .done = xSemaphoreCreateBinary()};
    if (!r.done) return;
    defer_begin();
    if (xTaskCreatePinnedToCore(task_tramp, name, stack, &r, 5, NULL, (BaseType_t)core) != pdPASS) {
        defer_flush();
        printf("task %s: cannot create (stack %u)\n", name, (unsigned)stack);
        vSemaphoreDelete(r.done);
        return;
    }
    xSemaphoreTake(r.done, portMAX_DELAY);
    vSemaphoreDelete(r.done);
    defer_flush();
}

/* ---- SD card ------------------------------------------------------------------------------------------------------ */

static int card_khz(void)
{
    const sdmmc_card_t *c = kiban_sd_card();
    return c ? c->real_freq_khz : 0;
}

static int sd_mount(int khz)
{
    if (kiban_sd_mount_point()) kiban_sd_unmount();
    const esp_err_t err = kiban_sd_mount(MNT, khz > 0 ? khz : 0);
    if (err != ESP_OK) {
        printf("mount: %s\n", esp_err_to_name(err));
        return -1;
    }
    printf("mounted %s, requested %d kHz, real %d kHz\n", MNT, khz > 0 ? khz : 20000, card_khz());
    return 0;
}

/* SD suite buffers: internal = DMA-capable, 64-byte aligned (no bounce copy in the SDMMC driver); PSRAM = plain */
enum { MEM_EXT = 0, MEM_INT = 1 };

static void *buf_alloc(size_t n, int where)
{
    if (where == MEM_INT) return heap_caps_aligned_alloc(64, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
}

/* ---- platform hooks of bench_core.h ------------------------------------------------------------------------------ */

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t ticks(void) { return (uint32_t)esp_cpu_get_cycle_count(); }
static void quiet_begin(void) { portENTER_CRITICAL(&s_mux); }
static void quiet_end(void) { portEXIT_CRITICAL(&s_mux); }
static void pause_(void) { vTaskDelay(1); }

static void *alloc32(size_t n, int where)
{
    return heap_caps_aligned_alloc(32, n, where == BENCH_INT ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : MALLOC_CAP_SPIRAM);
}

static void *malloc_cls(size_t n, int where)
{
    return heap_caps_malloc(n, where == BENCH_INT ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : MALLOC_CAP_SPIRAM);
}

static void heap_info(int where, size_t *f, size_t *l)
{
    const uint32_t caps = where == BENCH_INT ? MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT : MALLOC_CAP_SPIRAM;
    *f = heap_caps_get_free_size(caps);
    *l = heap_caps_get_largest_free_block(caps);
}

/* the third read class: 128 KB of flash .rodata, read through the data cache it shares with PSRAM */
static const uint8_t s_ro[128 * 1024] = {1, 2, 3, 4};

static bench_env s_env;
static int s_reps = 7;
static char s_file[256]; /* the optional user file of the sd suite ("" = none) */

static void env_init(void)
{
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_env = (bench_env){.ticks = ticks,
                        .ticks_per_ns = esp_rom_get_cpu_ticks_per_us() / 1000.0,
                        .ticks_are_cycles = 1,
                        .quiet_begin = quiet_begin,
                        .quiet_end = quiet_end,
                        .pause = pause_,
                        .alloc = alloc32,
                        .free = heap_caps_free,
                        .malloc_cls = malloc_cls,
                        .free_cls = heap_caps_free,
                        .heap_info = heap_info,
                        .int_max = largest > 32768 ? largest - 24576 : largest / 4,
                        .reps = s_reps,
                        .mem_name = {"int", "psram"}};
    bench_ro_region = s_ro;
    bench_ro_size = sizeof s_ro;
    bench_ro_name = "rodata";
}

/* ---- env line ------------------------------------------------------------------------------------------------------ */

static void bench_env_line(const char *suite)
{
    /* the cycle counter's rate against esp_timer over 10 ms (busy wait) */
    const int64_t t0 = esp_timer_get_time();
    const uint32_t c0 = ticks();
    while (esp_timer_get_time() - t0 < 10000) {
    }
    const uint32_t c1 = ticks();
    const int64_t t1 = esp_timer_get_time();
    /* SPI0 (the cache's flash port): the read mode in effect, whoever set it */
    const uint32_t ctrl = REG_READ(SPI_MEM_CTRL_REG(0)), clk = REG_READ(SPI_MEM_CLOCK_REG(0));
    const char *fmode = (ctrl & SPI_MEM_FREAD_QIO)    ? "qio"
                        : (ctrl & SPI_MEM_FREAD_QUAD) ? "qout"
                        : (ctrl & SPI_MEM_FREAD_DIO)  ? "dio"
                        : (ctrl & SPI_MEM_FREAD_DUAL) ? "dout"
                                                      : "slow";
#if CONFIG_COMPILER_OPTIMIZATION_PERF
    const char *opt = "O2";
#elif CONFIG_COMPILER_OPTIMIZATION_SIZE
    const char *opt = "Os";
#else
    const char *opt = "other";
#endif
    bench_emit("phase=env\tsuite=%s\tcpu_mhz=%u\tcpu_mhz_measured=%.1f\tcore=%d\topt=%s\tidf=%s\treps=%d"
               "\tpsram_mode=%s\tpsram_mhz_cfg=%d\tpsram_total=%u\tdcache_cfg=%d\tdcache_line=%d\tdcache_ways=%d"
               "\ticache_cfg=%d\ticache_line=%d\ticache_ways=%d\tflash_mode_cfg=%s\tflash_freq_cfg=%s"
               "\tflash_mode_reg=%s\tspi0_ctrl=%08x\tspi0_clock=%08x\tsd_khz=%d\tint_free=%u\tint_largest=%u"
               "\tpsram_free=%u\tpsram_largest=%u\tmalloc_alwaysinternal=%d\trodata_in_drom=%d",
               suite, (unsigned)esp_rom_get_cpu_ticks_per_us(), (double)(c1 - c0) / (double)(t1 - t0),
               (int)xPortGetCoreID(), opt, esp_get_idf_version(), s_reps,
#if CONFIG_SPIRAM_MODE_OCT
               "oct",
#else
               "quad",
#endif
               CONFIG_SPIRAM_SPEED, (unsigned)heap_caps_get_total_size(MALLOC_CAP_SPIRAM),
               CONFIG_ESP32S3_DATA_CACHE_SIZE, CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE,
               CONFIG_ESP32S3_DCACHE_ASSOCIATED_WAYS, CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE,
               CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE, CONFIG_ESP32S3_ICACHE_ASSOCIATED_WAYS,
               CONFIG_ESPTOOLPY_FLASHMODE, CONFIG_ESPTOOLPY_FLASHFREQ, fmode, (unsigned)ctrl, (unsigned)clk,
               card_khz(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM), CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL,
               esp_ptr_in_drom(s_ro) ? 1 : 0);
}

/* ---- code: flash vs IRAM, I-cache thrash ------------------------------------------------------------------------- */

/* one op = a dependent multiply-add with a distinct small immediate (≈ 5–6 bytes of code) */
#define S1(i)                                                                                                        \
    x = x * k + ((i) & 0x7Fu);                                                                                       \
    OPQ(x);
#define S4(i) S1(i) S1((i) + 1) S1((i) + 2) S1((i) + 3)
#define S16(i) S4(i) S4((i) + 4) S4((i) + 8) S4((i) + 12)
#define S64(i) S16(i) S16((i) + 16) S16((i) + 32) S16((i) + 48)
#define S256(i) S64(i) S64((i) + 64) S64((i) + 128) S64((i) + 192)
#define S1024(i) S256(i) S256((i) + 256) S256((i) + 512) S256((i) + 768)
#define S4096(i) S1024(i) S1024((i) + 1024) S1024((i) + 2048) S1024((i) + 3072)

/* 4,096 ops straight-line: larger than the 16 KB I-cache, so every call refetches its lines from flash */
NOINL static uint32_t code_big(uint32_t x, uint32_t k)
{
    S4096(0)
    return x;
}

/* the same op stream as a 256-op loop body (≈ 1.4 KB), in flash and in IRAM */
NOINL static uint32_t code_small_flash(uint32_t x, uint32_t k, uint32_t n)
{
    for (uint32_t r = 0; r < n; r++) {
        S256(0)
    }
    return x;
}

NOINL IRAM_ATTR static uint32_t code_small_iram(uint32_t x, uint32_t k, uint32_t n)
{
    for (uint32_t r = 0; r < n; r++) {
        S256(0)
    }
    return x;
}

static volatile uint32_t s_k = 0x9E3779B1u;

static void suite_code(void)
{
    const bench_env *e = &s_env;
    uint32_t t[32], v = 0;
    const uint32_t k = s_k;
    const int reps = s_reps < 3 ? 3 : s_reps > 31 ? 31 : s_reps;
    /* loops of 65,536 ops each */
    for (int r = 0; r < reps; r++) {
        quiet_begin();
        const uint32_t t0 = ticks();
        for (int i = 0; i < 16; i++) v = code_big(v, k);
        t[r] = ticks() - t0;
        quiet_end();
    }
    pause_();
    bench_report(e, "code", "madd_stream", "where=flash\tbody=straight_4096", 65536, "op", t, v);
    for (int r = 0; r < reps; r++) {
        quiet_begin();
        const uint32_t t0 = ticks();
        v = code_small_flash(v, k, 256);
        t[r] = ticks() - t0;
        quiet_end();
    }
    pause_();
    bench_report(e, "code", "madd_stream", "where=flash\tbody=loop_256", 65536, "op", t, v);
    for (int r = 0; r < reps; r++) {
        quiet_begin();
        const uint32_t t0 = ticks();
        v = code_small_iram(v, k, 256);
        t[r] = ticks() - t0;
        quiet_end();
    }
    pause_();
    bench_report(e, "code", "madd_stream", "where=iram\tbody=loop_256", 65536, "op", t, v);
    /* one pass of the 256-op body, cold (right after code_big flushed the I-cache) and warm (called again) */
    uint32_t tc[32], tw[32], ti[32];
    for (int r = 0; r < reps; r++) {
        quiet_begin();
        v = code_big(v, k);
        uint32_t t0 = ticks();
        v = code_small_flash(v, k, 1);
        tc[r] = ticks() - t0;
        t0 = ticks();
        v = code_small_flash(v, k, 1);
        tw[r] = ticks() - t0;
        v = code_big(v, k);
        t0 = ticks();
        v = code_small_iram(v, k, 1);
        ti[r] = ticks() - t0;
        quiet_end();
    }
    pause_();
    bench_report(e, "code", "call_256ops", "where=flash\tstate=cold_icache", 256, "op", tc, v);
    bench_report(e, "code", "call_256ops", "where=flash\tstate=warm", 256, "op", tw, v);
    bench_report(e, "code", "call_256ops", "where=iram\tstate=after_thrash", 256, "op", ti, v);
}

/* ---- sd ---------------------------------------------------------------------------------------------------------- */

#define SD_RAND_N 128
#define SD_DIR_FILES 128

static int cmp_i64(const void *a, const void *b)
{
    const int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

/* "avg_us=… med_us=… p95_us=… max_us=…" of n samples (sorted in place) */
static void dist(char *out, size_t cap, int64_t *us, int n)
{
    int64_t sum = 0;
    for (int i = 0; i < n; i++) sum += us[i];
    qsort(us, (size_t)n, sizeof us[0], cmp_i64);
    const int p95 = (n * 95) / 100 < n ? (n * 95) / 100 : n - 1;
    snprintf(out, cap, "n=%d\tavg_us=%.1f\tmed_us=%lld\tp95_us=%lld\tmax_us=%lld\tmin_us=%lld", n,
             n ? (double)sum / n : 0.0, (long long)us[n / 2], (long long)us[p95], (long long)us[n - 1],
             (long long)us[0]);
}

static const char *bufname(const void *p) { return esp_ptr_external_ram(p) ? "psram" : "int"; }

static void sd_cleanup(void)
{
    char path[96];
    unlink(BENCH_DIR "/w.bin");
    unlink(BENCH_DIR "/r.bin");
    unlink(BENCH_DIR "/s.bin");
    for (int i = 0; i < SD_DIR_FILES; i++) {
        snprintf(path, sizeof path, BENCH_DIR "/d/f%03d.bin", i);
        unlink(path);
    }
    rmdir(BENCH_DIR "/d");
    rmdir(BENCH_DIR);
}

static uint32_t s_rng = 0x5EED1234u;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static int sd_write_file(const char *path, const uint8_t *buf, size_t block, size_t total, const char *name)
{
    int64_t t0 = esp_timer_get_time();
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int64_t t1 = esp_timer_get_time();
    if (fd < 0) {
        bench_emit("suite=sd\tname=error\treason=create\terrno=%d", errno);
        return -1;
    }
    size_t done = 0;
    while (done < total) {
        if (write(fd, buf, block) != (ssize_t)block) break;
        done += block;
    }
    const int64_t t2 = esp_timer_get_time();
    const int sr = fsync(fd);
    const int64_t t3 = esp_timer_get_time();
    close(fd);
    const int64_t t4 = esp_timer_get_time();
    bench_emit("suite=sd\tname=%s\tsd_khz=%d\tblock=%u\tbuf=%s\tbytes=%u\topen_us=%lld\twrite_us=%lld\tfsync_us=%lld"
               "\tclose_us=%lld\tmb_s=%.3f\tmb_s_with_fsync=%.3f\tok=%d",
               name, card_khz(), (unsigned)block, bufname(buf), (unsigned)done, (long long)(t1 - t0),
               (long long)(t2 - t1), (long long)(t3 - t2), (long long)(t4 - t3),
               (t2 - t1) ? (double)done / (double)(t2 - t1) : 0.0, (t3 - t1) ? (double)done / (double)(t3 - t1) : 0.0,
               done == total && sr == 0);
    return done == total && sr == 0 ? 0 : -1;
}

static void sd_seq_read(const char *path, uint8_t *buf, size_t block, size_t total, size_t off)
{
    const int64_t t0 = esp_timer_get_time();
    const int fd = open(path, O_RDONLY);
    const int64_t t1 = esp_timer_get_time();
    if (fd < 0) return;
    lseek(fd, (off_t)off, SEEK_SET);
    size_t done = 0;
    uint32_t calls = 0;
    const int64_t t2 = esp_timer_get_time();
    while (done < total) {
        const ssize_t r = read(fd, buf, block);
        calls++;
        if (r <= 0) break;
        done += (size_t)r;
    }
    const int64_t t3 = esp_timer_get_time();
    close(fd);
    bench_emit("suite=sd\tname=seq_read\tsd_khz=%d\tblock=%u\tbuf=%s\tbytes=%u\tcalls=%u\topen_us=%lld\tread_us=%lld"
               "\tus_per_call=%.1f\tmb_s=%.3f\tok=%d",
               card_khz(), (unsigned)block, bufname(buf), (unsigned)done, (unsigned)calls, (long long)(t1 - t0),
               (long long)(t3 - t2), calls ? (double)(t3 - t2) / calls : 0.0,
               (t3 - t2) ? (double)done / (double)(t3 - t2) : 0.0, done == total);
}

/* SD_RAND_N reads of len at random offsets (aligned to align, or odd when align == 1) of an open file */
static void sd_rand_read(const char *label, int fd, size_t size, uint8_t *buf, size_t len, size_t align, int64_t *us)
{
    int bad = 0;
    for (int i = 0; i < SD_RAND_N; i++) {
        size_t off = (size_t)rnd() % (size - len);
        off = align > 1 ? off / align * align : off | 1u;
        const int64_t t0 = esp_timer_get_time();
        const off_t o = lseek(fd, (off_t)off, SEEK_SET);
        const ssize_t r = read(fd, buf, len);
        us[i] = esp_timer_get_time() - t0;
        bad += o != (off_t)off || r != (ssize_t)len;
    }
    char d[160];
    dist(d, sizeof d, us, SD_RAND_N);
    bench_emit("suite=sd\tname=rand_read\tsd_khz=%d\tfile=%s\tsize=%u\tlen=%u\talign=%u\tbuf=%s\t%s\tok=%d", card_khz(),
               label, (unsigned)size, (unsigned)len, (unsigned)align, bufname(buf), d, bad == 0);
}

static void suite_sd(void)
{
    if (!kiban_sd_mount_point()) {
        bench_emit("suite=sd\tname=error\treason=not_mounted");
        return;
    }
    uint64_t total = 0, freeb = 0;
    if (esp_vfs_fat_info(MNT, &total, &freeb) != ESP_OK || freeb < 32ull * 1024 * 1024) {
        bench_emit("suite=sd\tname=error\treason=free_space\tfree=%llu", (unsigned long long)freeb);
        return;
    }
    size_t ib = 64 * 1024;
    uint8_t *bi = buf_alloc(ib, MEM_INT);
    if (!bi) bi = buf_alloc(ib = 32 * 1024, MEM_INT);
    uint8_t *bp = buf_alloc(64 * 1024, MEM_EXT);
    int64_t *us = heap_caps_malloc(sizeof(int64_t) * (SD_DIR_FILES > SD_RAND_N ? SD_DIR_FILES : SD_RAND_N),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    struct stat st;
    if (!bi || !bp || !us) {
        bench_emit("suite=sd\tname=error\treason=no_memory");
        goto out_mem;
    }
    for (size_t i = 0; i < ib; i++) bi[i] = (uint8_t)rnd();
    memcpy(bp, bi, ib < 65536 ? ib : 65536);
    bench_emit("suite=sd\tname=setup\tsd_khz=%d\tfree_before=%llu\tint_buf=%u", card_khz(), (unsigned long long)freeb,
               (unsigned)ib);
    sd_cleanup(); /* leftovers of an interrupted run */
    if (mkdir(BENCH_DIR, 0777) != 0) {
        bench_emit("suite=sd\tname=error\treason=mkdir\terrno=%d", errno);
        goto out;
    }
    char d[160];

    /* writes at block sizes, each to a fresh file that is deleted again (unlink timed) */
    static const struct {
        size_t block, total;
        int ext;
    } wr[] = {{512, 256 * 1024, 0}, {4096, 1024 * 1024, 0}, {16384, 1024 * 1024, 0}, {65536, 1024 * 1024, 0},
              {4096, 1024 * 1024, 1}, {65536, 1024 * 1024, 1}};
    for (size_t i = 0; i < sizeof wr / sizeof wr[0]; i++) {
        if (!wr[i].ext && wr[i].block > ib) continue;
        if (sd_write_file(BENCH_DIR "/w.bin", wr[i].ext ? bp : bi, wr[i].block, wr[i].total, "write")) goto out;
        const int64_t t0 = esp_timer_get_time();
        unlink(BENCH_DIR "/w.bin");
        bench_emit("suite=sd\tname=unlink\tsize=%u\tus=%lld", (unsigned)wr[i].total,
                   (long long)(esp_timer_get_time() - t0));
        pause_();
    }
    /* the 4 MB read file */
    const size_t rsize = 4 * 1024 * 1024;
    if (sd_write_file(BENCH_DIR "/r.bin", bi, ib, rsize, "write_4m")) goto out;

    /* sequential reads: 1 MB (256 KB below 4 KB blocks) from a different MB of the file each time */
    static const size_t blocks[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
    for (size_t i = 0; i < sizeof blocks / sizeof blocks[0]; i++) {
        if (blocks[i] > ib) continue;
        sd_seq_read(BENCH_DIR "/r.bin", bi, blocks[i], blocks[i] < 4096 ? 256 * 1024 : 1024 * 1024,
                    (i % 4) * 1024 * 1024);
        pause_();
    }
    sd_seq_read(BENCH_DIR "/r.bin", bp, 4096, 1024 * 1024, 0);
    sd_seq_read(BENCH_DIR "/r.bin", bp, 65536, 1024 * 1024, 1024 * 1024);

    /* random reads: on the 4 MB file (O_RDONLY: fast seek) and on the user file, read-only */
    int fd = open(BENCH_DIR "/r.bin", O_RDONLY);
    if (fd >= 0) {
        sd_rand_read("scratch_4m", fd, rsize, bi, 4096, 4096, us);
        sd_rand_read("scratch_4m", fd, rsize, bi, 4096, 1, us);
        sd_rand_read("scratch_4m", fd, rsize, bi, 64, 1, us);
        sd_rand_read("scratch_4m", fd, rsize, bp, 4096, 4096, us);
        close(fd);
    }
    const int have_user = s_file[0] && stat(s_file, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 65536;
    if (have_user) {
        fd = open(s_file, O_RDONLY);
        if (fd >= 0) {
            sd_rand_read("user", fd, (size_t)st.st_size, bi, 4096, 4096, us);
            sd_rand_read("user", fd, (size_t)st.st_size, bi, 4096, 1, us);
            sd_rand_read("user", fd, (size_t)st.st_size, bi, 64, 1, us);
            close(fd);
        }
        bench_emit("suite=sd\tname=user_file\tsize=%ld", (long)st.st_size);
    } else {
        bench_emit("suite=sd\tname=user_file\tsize=0\tnote=%s", s_file[0] ? "missing_or_too_small" : "none_given");
    }
    pause_();

    /* lseek alone, 1,000 random targets: read-only open (fast seek map) and read-write open (cluster-chain walk) */
    static const int modes[2] = {O_RDONLY, O_RDWR};
    for (int m = 0; m < 2; m++) {
        fd = open(BENCH_DIR "/r.bin", modes[m]);
        if (fd < 0) continue;
        int bad = 0;
        const int64_t t0 = esp_timer_get_time();
        for (int i = 0; i < 1000; i++) bad += lseek(fd, (off_t)(rnd() % rsize), SEEK_SET) < 0;
        const int64_t t1 = esp_timer_get_time();
        for (int i = 0; i < SD_RAND_N; i++) {
            const size_t off = (size_t)rnd() % (rsize - 1);
            const int64_t a = esp_timer_get_time();
            bad += lseek(fd, (off_t)off, SEEK_SET) != (off_t)off || read(fd, bi, 1) != 1;
            us[i] = esp_timer_get_time() - a;
        }
        close(fd);
        dist(d, sizeof d, us, SD_RAND_N);
        bench_emit("suite=sd\tname=lseek\topen=%s\tcalls=1000\tus_per_call=%.2f\tok=%d", m ? "rdwr" : "rdonly",
                   (double)(t1 - t0) / 1000.0, bad == 0);
        bench_emit("suite=sd\tname=lseek_read1\topen=%s\t%s\tok=%d", m ? "rdwr" : "rdonly", d, bad == 0);
    }

    /* fsync: 32 × (append 4 KB + fsync), then 32 × fsync with nothing pending */
    fd = open(BENCH_DIR "/s.bin", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        int bad = 0;
        int64_t w[32];
        for (int i = 0; i < 32; i++) {
            const int64_t a = esp_timer_get_time();
            bad += write(fd, bi, 4096) != 4096;
            const int64_t b = esp_timer_get_time();
            bad += fsync(fd) != 0;
            us[i] = esp_timer_get_time() - b;
            w[i] = b - a;
        }
        dist(d, sizeof d, us, 32);
        bench_emit("suite=sd\tname=fsync\tafter=append_4k\t%s\tok=%d", d, bad == 0);
        dist(d, sizeof d, w, 32);
        bench_emit("suite=sd\tname=append_4k\t%s", d);
        for (int i = 0; i < 32; i++) {
            const int64_t a = esp_timer_get_time();
            bad += fsync(fd) != 0;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, 32);
        bench_emit("suite=sd\tname=fsync\tafter=nothing\t%s\tok=%d", d, bad == 0);
        close(fd);
        unlink(BENCH_DIR "/s.bin");
    }
    pause_();

    /* open + close, stat, misses: 50 each */
    static const struct {
        const char *label;
        int user;
        int mode;
    } oc[] = {{"scratch_4m_rdonly", 0, O_RDONLY}, {"scratch_4m_rdwr", 0, O_RDWR}, {"user_rdonly", 1, O_RDONLY}};
    for (size_t k = 0; k < sizeof oc / sizeof oc[0]; k++) {
        if (oc[k].user && !have_user) continue;
        const char *p = oc[k].user ? s_file : BENCH_DIR "/r.bin";
        int64_t cl[50];
        int bad = 0;
        for (int i = 0; i < 50; i++) {
            const int64_t a = esp_timer_get_time();
            const int f = open(p, oc[k].mode);
            const int64_t b = esp_timer_get_time();
            bad += f < 0;
            if (f >= 0) close(f);
            us[i] = b - a;
            cl[i] = esp_timer_get_time() - b;
        }
        dist(d, sizeof d, us, 50);
        bench_emit("suite=sd\tname=open\tfile=%s\t%s\tok=%d", oc[k].label, d, bad == 0);
        dist(d, sizeof d, cl, 50);
        bench_emit("suite=sd\tname=close\tfile=%s\t%s", oc[k].label, d);
    }
    {
        int bad = 0;
        for (int i = 0; i < 50; i++) {
            const int64_t a = esp_timer_get_time();
            bad += stat(BENCH_DIR "/r.bin", &st) != 0;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, 50);
        bench_emit("suite=sd\tname=stat\tfile=scratch_4m\t%s\tok=%d", d, bad == 0);
        for (int i = 0; i < 50; i++) {
            const int64_t a = esp_timer_get_time();
            bad += stat(BENCH_DIR "/missing.bin", &st) == 0;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, 50);
        bench_emit("suite=sd\tname=stat\tfile=missing_in_bench_dir\t%s\tok=%d", d, bad == 0);
    }
    pause_();

    /* a directory of SD_DIR_FILES empty files: create, scan, stat each, miss, unlink */
    if (mkdir(BENCH_DIR "/d", 0777) == 0) {
        char path[96];
        int bad = 0;
        for (int i = 0; i < SD_DIR_FILES; i++) {
            snprintf(path, sizeof path, BENCH_DIR "/d/f%03d.bin", i);
            const int64_t a = esp_timer_get_time();
            const int f = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (f >= 0) close(f);
            else bad++;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, SD_DIR_FILES);
        bench_emit("suite=sd\tname=dir_create\tfiles=%d\t%s\tok=%d", SD_DIR_FILES, d, bad == 0);
        int64_t scan[5];
        int seen = 0;
        for (int r = 0; r < 5; r++) {
            const int64_t a = esp_timer_get_time();
            DIR *dp = opendir(BENCH_DIR "/d");
            seen = 0;
            if (dp) {
                while (readdir(dp)) seen++;
                closedir(dp);
            }
            scan[r] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, scan, 5);
        bench_emit("suite=sd\tname=dir_scan\tentries=%d\t%s\tok=%d", seen, d, seen == SD_DIR_FILES);
        for (int i = 0; i < SD_DIR_FILES; i++) {
            snprintf(path, sizeof path, BENCH_DIR "/d/f%03d.bin", (i * 37) % SD_DIR_FILES);
            const int64_t a = esp_timer_get_time();
            bad += stat(path, &st) != 0;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, SD_DIR_FILES);
        bench_emit("suite=sd\tname=dir_stat\tfiles=%d\t%s\tok=%d", SD_DIR_FILES, d, bad == 0);
        for (int i = 0; i < 50; i++) {
            const int64_t a = esp_timer_get_time();
            const int f = open(BENCH_DIR "/d/missing.bin", O_RDONLY);
            if (f >= 0) close(f);
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, 50);
        bench_emit("suite=sd\tname=dir_open_missing\tfiles=%d\t%s", SD_DIR_FILES, d);
        for (int i = 0; i < SD_DIR_FILES; i++) {
            snprintf(path, sizeof path, BENCH_DIR "/d/f%03d.bin", i);
            const int64_t a = esp_timer_get_time();
            bad += unlink(path) != 0;
            us[i] = esp_timer_get_time() - a;
        }
        dist(d, sizeof d, us, SD_DIR_FILES);
        bench_emit("suite=sd\tname=dir_unlink\tfiles=%d\t%s\tok=%d", SD_DIR_FILES, d, bad == 0);
    }
out:
    sd_cleanup();
    {
        uint64_t f2 = 0;
        esp_vfs_fat_info(MNT, &total, &f2);
        bench_emit("suite=sd\tname=cleanup\tbench_dir_gone=%d\tfree_before=%llu\tfree_after=%llu\tok=%d",
                   stat(BENCH_DIR, &st) != 0, (unsigned long long)freeb, (unsigned long long)f2,
                   stat(BENCH_DIR, &st) != 0 && f2 == freeb);
    }
out_mem:
    heap_caps_free(bi);
    heap_caps_free(bp);
    heap_caps_free(us);
}

/* ---- commands ---------------------------------------------------------------------------------------------------- */

typedef struct {
    const char *name;
    int bad;
} job;

static void bench_task(void *p)
{
    job *j = p;
    env_init();
    bench_env_line(j->name);
    const int64_t t0 = esp_timer_get_time();
    if (!strcmp(j->name, "cpu")) j->bad = bench_cpu(&s_env);
    else if (!strcmp(j->name, "mem")) j->bad = bench_mem(&s_env);
    else if (!strcmp(j->name, "chase")) j->bad = bench_chase(&s_env);
    else if (!strcmp(j->name, "stride")) j->bad = bench_stride(&s_env);
    else if (!strcmp(j->name, "struct")) j->bad = bench_struct(&s_env);
    else if (!strcmp(j->name, "alloc")) j->bad = bench_alloc(&s_env);
    else if (!strcmp(j->name, "code")) suite_code();
    else if (!strcmp(j->name, "sd")) suite_sd();
    bench_emit("phase=suite_done\tsuite=%s\tbad=%d\tms=%lld", j->name, j->bad,
               (long long)((esp_timer_get_time() - t0) / 1000));
}

static const char *const k_suites[] = {"cpu", "mem", "chase", "stride", "struct", "alloc", "code", "sd"};
#define NSUITES (sizeof k_suites / sizeof k_suites[0])

static int cmd_bench(int argc, char **argv)
{
    int want[NSUITES] = {0}, any = 0;
    s_reps = 7;
    s_file[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "reps=", 5)) {
            s_reps = atoi(argv[i] + 5);
            if (s_reps < 3) s_reps = 3;
            if (s_reps > 31) s_reps = 31;
            continue;
        }
        if (!strncmp(argv[i], "file=", 5)) {
            if (strlen(argv[i] + 5) >= sizeof s_file) {
                printf("file: path too long\n");
                return 1;
            }
            strcpy(s_file, argv[i] + 5);
            continue;
        }
        int found = 0;
        for (size_t k = 0; k < NSUITES; k++)
            if (!strcmp(argv[i], "all") || !strcmp(argv[i], k_suites[k])) {
                want[k] = 1;
                found = any = 1;
            }
        if (!found) {
            printf("unknown suite: %s\n", argv[i]);
            return 1;
        }
    }
    if (!any)
        for (size_t k = 0; k < NSUITES; k++) want[k] = 1;
    const int64_t t0 = esp_timer_get_time();
    int bad = 0;
    for (size_t k = 0; k < NSUITES; k++) {
        if (!want[k]) continue;
        job j = {.name = k_suites[k]};
        run_task_core("bench", BENCH_STACK, bench_task, &j, BENCH_CORE);
        bad += j.bad;
    }
    bench_emit("phase=done\tbad=%d\tms=%lld", bad, (long long)((esp_timer_get_time() - t0) / 1000));
    return 0;
}

static int cmd_mount(int argc, char **argv)
{
    return sd_mount(argc > 1 ? atoi(argv[1]) : 0) ? 1 : 0;
}

static int cmd_reboot(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    esp_restart();
    return 0;
}

void app_main(void)
{
    kiban_board_cfg_t bcfg = KIBAN_BOARD_CFG_DEFAULT();
    const esp_err_t err = kiban_board_init(&bcfg);
    if (err != ESP_OK) ESP_LOGE(TAG, "board init failed: %s (console still starts)", esp_err_to_name(err));
    /* If the stock bootloader has rollback enabled and this image is pending, confirm it. */
    esp_ota_mark_app_valid_cancel_rollback();
    /* Give the USB PHY to USB-Serial/JTAG in case a reset left it on OTG (USB drive mode). */
    kiban_usb_drive_stop();
    sd_mount(0);

    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "bench> ";
    rc.task_stack_size = 8192;
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        {.command = "bench",
         .help = "bench [all|cpu|mem|chase|stride|struct|alloc|code|sd ...] [reps=N] [file=<path>]: device "
                 "performance baseline (docs/research/004), \"@BENCH\" lines; each suite pinned to core 1, interrupts "
                 "off per timed repetition; sd writes only below " BENCH_DIR " and removes it; file= is an optional "
                 "existing file (> 64 KB) read O_RDONLY for random reads",
         .func = cmd_bench},
        {.command = "mount",
         .help = "mount [khz]: remount the card at " MNT " (20000 default, 40000 high speed)",
         .func = cmd_mount},
        {.command = "reboot", .help = "software reset", .func = cmd_reboot},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}
