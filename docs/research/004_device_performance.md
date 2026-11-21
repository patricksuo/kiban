# 004 · Device performance baseline (CPU, memory hierarchy, allocator, code placement, SD card)

Per-operation costs of the Metalio E-Ink 4 (ESP32-S3 Xtensa LX7 dual core at 240 MHz, 8 MB octal PSRAM, 16 MB
flash, microSD over SDMMC 1-bit with FatFs) under ESP-IDF v6.0.2, so that a slow stage of a product can be explained
as "N cache misses × M cycles" or "N soft-double ops × K cycles" instead of guessed. Tool: `examples/bench`.
Run on one board, 2026-11-21.

## Provenance of the numbers

- The §4 results were measured with the same bench sources built into the console test app of a reader firmware
  (IDF v6.0.2, `-O2`). `examples/bench` at kiban `ef2c20c` is that code with two kinds of change only: the output hook
  (`bench_emit` instead of the app's own result-line function) and the sd suite's paths (scratch directory, the
  random-read file taken from `file=` instead of picked on the card). That app used the kiban 0.3.0 components for
  board bring-up and the SD mount. The generated `sdkconfig` of `examples/bench`
  equals that app's except for the partition-table path (diff of the two files) `[measured]`.
- Code generation `[source]` (`xtensa-esp32s3-elf-nm -S` / `objdump -d` of both images; the measured image's ELF is
  identified by the SHA-256 prefix in its boot log): every portable kernel and
  the three `code` functions have the same size in both builds, with one exception: `k_call` (the `call_direct`
  line) reached its leaf by `l32r` + `callx8` in the measured image (the leaf was out of `call8` range) and by a
  direct `call8` in `examples/bench`. Expect `call_direct` to differ slightly.
- The host driver built from this tree reproduces the checksums (`chk=`) of the host run used below for every
  kernel line except `chase` (its checksum is a node address) `[measured]`.
- A run of the `examples/bench` binary itself (at b207a3c, `mount 40000` then `bench`) gives the same median cycles
  within 5 % on all 92 kernel lines the two runs share, except `call_direct` (16.0 → 13.0 cycles, the direct
  `call8` above) and `call_256ops` (warm 3.82 → 3.04, after thrash 4.68 → 4.33); all suites `bad=0`, 21.1 s in all
  `[measured]`.

## 1. Platform facts the benchmark depends on

| Item | Value | Evidence |
|---|---|---|
| CPU | 240 MHz (`CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`); cycle counter against `esp_timer`: 240.0 MHz | config; `[measured]` |
| Optimisation | `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`) | config |
| Instruction cache | 16 KB, 8-way, 32 B lines | config; applied at app start `[source] components/esp_system/port/cpu_start.c:532` |
| Data cache | 32 KB, 8-way, 32 B lines; serves PSRAM and flash `.rodata` (the 128 KB test array is in DROM: `rodata_in_drom=1`), not internal SRAM | config, `cpu_start.c:538`; users `[measured]` (§4.3, §4.4) |
| Caches and the two cores | **One I-cache and one D-cache shared by both cores**, each core on its own bus into them: ESP32-S3 does not define `SOC_IDCACHE_PER_CORE` (only ESP32 does, `components/soc/esp32/include/soc/soc_caps.h:162`); the flash-operation path disables the other core's cache only under that macro (`components/spi_flash/cache_utils.c:187-191`); a single `EXTMEM_DCACHE_CTRL1_REG` / `EXTMEM_ICACHE_CTRL1_REG` carries `..._SHUT_CORE0_BUS` / `..._SHUT_CORE1_BUS` (`components/soc/esp32s3/register/soc/extmem_reg.h:36-45,397-400`); "Both cores have their own IBUS0 / DBUS0" (`components/hal/esp32s3/include/hal/cache_ll.h:564-566`). Consequence: code or data touched on core 0 evicts lines that core 1 uses | `[source]`; consequence `[inference]`; the TRM itself not checked |
| PSRAM | octal, 80 MHz; `malloc` ≤ 16,384 B goes to internal RAM (`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384`) | config |
| Flash read mode | configured DIO 80 MHz, but **SPI0 (the cache's flash port) runs QIO**: `SPI_MEM_CTRL_REG(0)` = `0x012c2008`, bit 24 `SPI_MEM_FREAD_QIO` set; `SPI_MEM_CLOCK_REG(0)` = `0x00010001` | register `[measured]`; bit `[source] components/soc/esp32s3/register/soc/spi_mem_reg.h:139` |
| Why QIO | The device runs the vendor's second-stage bootloader, not ours. An IDF bootloader built for QIO writes `dio` into image headers (`components/esptool_py/Kconfig.projbuild:81`) and switches the chip to QIO at run time (`components/bootloader_support/bootloader_flash/src/bootloader_flash_config_esp32s3.c:288-290`); the app does not switch SPI0 back. So the boot log shows `mode:DIO` (ROM, from the header) and the app logs `flash io: dio` (its configured mode), while cache reads run in QIO | log lines `[measured]`; explanation `[inference]` |
| SD | SDMMC 1-bit (kiban pins), 40 MHz for the run (`mount 40000`; real 40,000 kHz); FatFs fast seek for read-only opens (`CONFIG_FATFS_USE_FASTSEEK`), sector buffers internal (`CONFIG_FATFS_ALLOC_PREFER_EXTRAM=n`); card model not recorded | config, `[measured]` |
| Heap | TLSF allocator behind `heap_caps` | `[source] components/heap/CMakeLists.txt:17-26` |
| FPU | single-precision add / mul / multiply-add are hardware (`add.s` / `mul.s` / `madd.s`); **float divide is a call** of the ROM's `__divsf3`; **`double` is soft-float throughout** (ROM `__adddf3`, `__muldf3`, …) | `[source]` disassembly of the bench build |
| Integer divide | 32-bit divide is an instruction; **64-bit divide is a call** of the ROM's `__udivdi3` | same disassembly |
| Library calls | `memcpy` and `strcmp` are the ROM's; a struct assignment of 64 B and of 1,024 B compiles to a call of that `memcpy` | same disassembly |
| Calls | windowed ABI (`entry` / `call8` / `retw`); deep chains pay window overflow / underflow exceptions | ABI `[source]`; cost §4.1 |

## 2. Method

### 2.1 Harness

- Every suite runs in its own FreeRTOS task **pinned to core 1** (16 KB internal stack, priority 5). Its result
  lines are formatted into a PSRAM buffer and printed only after the task ends, so no console output falls inside a
  measurement. Each suite starts with a `phase=env` line and ends with `phase=suite_done` (wall time in ms).
- CPU / memory / code kernels: each measurement runs `reps` times (default 7, `reps=N` 3–31); each repetition is
  timed alone with the CPU cycle counter (`esp_cpu_get_cycle_count`, per core, 32-bit) **with interrupts masked on
  core 1** (`portENTER_CRITICAL`); the work per repetition is ~0.1–80 ms `[inference]`, below the 300 ms interrupt
  watchdog and the counter's 17.9 s wrap. After each measurement the task sleeps one tick so IDLE1 feeds the task
  watchdog. Core 0 keeps running the console and system tasks (and shares the caches, §1).
- A line reports per operation `cyc_min`, `cyc_med`, `cyc_max` over the repetitions, `ns_med` (= `cyc_med` / 0.24),
  `spread_pct` = (max − min) / min of the repetition times, `mb_s` for byte-unit lines, and `chk`, a checksum of the
  kernel results (the work cannot be optimised away; identical inputs give identical `chk` on device and host).
  Self-checks print `ok=1|0` (string results, UTF-8 char counts, the pointer-chase cycle, allocator leak, SD cleanup).
- Kernels are `noinline`, take sizes and inputs through a context or `volatile` seeds, and carry an empty asm on
  dependent values (no instruction) so steps cannot be folded; memory loops carry an empty asm memory clobber where
  the compiler would otherwise hoist a pure call or merge a copy loop into one `memcpy`. The disassembly of `k_alu`,
  `k_branch`, `k_fadd`, `k_fmul`, `k_fmadd`, `k_fdiv`, `k_dadd`, `k_div64`, `k_chase`, `k_st64`, `k_strcmp_matrix`
  was checked: the intended instructions / calls are in the loops `[source]`.
- **Host comparison**: `main/bench_core.c` has no ESP-IDF headers; `host/host_bench.c` runs the six portable suites on
  a host (`cc -O2`, ticks in ns; "int" and "psram" are both the plain heap). The device / host ratio is taken from
  `ns_med`. Host repetitions are short and an out-of-order core hides latencies, so the ratio per kernel is the
  interesting quantity, not the host figure. The host column below is an arm64 macOS machine (Apple silicon),
  `reps=15`.

### 2.2 Suites (`bench <suite ...>`; `all` = all, in this order)

**cpu** — cycles per operation, operands in registers unless stated:

| Line | One op |
|---|---|
| `loop_empty` | one iteration of an empty counted loop |
| `alu_dep` / `alu_indep4` | shift + xor + add, each step depending on the previous / the same step in four independent chains |
| `mul32_dep`, `div32_dep`, `mul64_dep`, `div64_dep` | dependent multiply-add / unsigned divide (+ and / or / add), 32- and 64-bit |
| `branch pattern=random/taken/alternate` | one data-dependent branch per byte over 4 KB of internal RAM (if-conversion blocked) |
| `float_{add,mul,madd,div}`, `double_{...}` | dependent chain of that operation |
| `call_inline`, `call_direct`, `call_indirect` | an add without a call; a call of a 2-argument leaf; the same through a function pointer |
| `call_recursive depth=1…32` | per call level of a non-tail recursion of that depth |
| `strcmp_matrix`, `strcmp_equal`, `memcmp8/16_equal`, `strlen`, `fnv1a` | on 32 short element / class names (`p`, `span`, `ruby`, `calibre12`, …), in internal RAM and in PSRAM: all 1,024 pairs (mostly a first-byte miss), equal strings at different addresses, equal 8 / 16-byte records, length, FNV-1a. The pools are 1 KB, so the PSRAM case is cache-resident: it measures PSRAM-through-cache hits |
| `utf8_decode` | a branchy UTF-8 decoder over generated Japanese-like text (45 % kanji, 30 % kana, 10 % CJK punctuation, 15 % ASCII; 2.7 B per char): 8 KB internal, 8 KB PSRAM (cache-resident), 256 KB PSRAM (streams); cycles per byte |

**mem** — bandwidth of `read` (word loads, 4 accumulators), `write` (word stores), `memset`, `memcpy`. Classes:
`int` (32 KB internal, re-read to 1 MB per repetition), `psram_hot8k` (8 KB of PSRAM, cache-resident), `psram`
(1 MB), `rodata` (128 KB of flash `.rodata`, read only). `memcpy`: int→int, PSRAM-hot→PSRAM-hot (4 KB),
PSRAM→PSRAM, PSRAM→int, int→PSRAM (1 MB in 32 KB chunks), rodata→int.

**chase** — latency: one random cycle (Sattolo) through every 32-byte node (one node per cache line) of a working
set, `p = *p`; cycles per hop. Internal 1–128 KB; PSRAM 1 KB–4 MB. `chase_rmw` also increments a word of each node
(evicted lines are dirty and written back). One 1 MB walk in address order (`order=seq`). Each working set is walked
once untimed first (warm-up, and the self-check that one lap returns to the start).

**stride** — one byte load every `s` bytes, `s` = 4…512, over 32 KB internal, 1 MB PSRAM and 128 KB `.rodata`, with
64 KB of other PSRAM read before each repetition (cold data cache).

**struct** — `d[i] = s[i]` for structs of 64, 256, 1,024 B: internal (2 × 4 KB), PSRAM hot (2 × 4 KB), PSRAM cold
(2 × 256 KB).

**alloc** — `heap_caps_malloc` + `heap_caps_free` with `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` (`caps_int`),
`MALLOC_CAP_SPIRAM` (`caps_psram`), and libc `malloc` / `free` (internal at these sizes); interrupts on (the heap
takes its own lock). `pair` = malloc then free ×1,000 at 16–4,096 B; `batch_*` = 256 mallocs of 64 / 1,024 B, then
their frees LIFO and FIFO; `frag` = blocks of random 16–512 B (PSRAM 2,048; internal bounded by free memory), every
other one freed, then `pair` of 32 B and 2 KB in that holed heap.

**code** — instruction fetch; one op = a dependent multiply-add with a distinct small immediate. `madd_stream`
65,536 ops as (a) a 4,096-op straight-line function in flash (24,005 B of code, larger than the I-cache, so every
call refetches every line), (b) a 256-op loop body in flash (1,510 B), (c) the same body in IRAM. `call_256ops`: one
pass of the 256-op body right after the big function evicted the I-cache (`cold_icache`), immediately again
(`warm`), and the IRAM copy after the same eviction.

**sd** — through the VFS (`open` / `read` / `write` / `lseek` / `fsync` / `stat` / `opendir`), times by `esp_timer`:
writes at 512 B–64 KB blocks from internal and PSRAM buffers (separate open / write / fsync / close times; `unlink`
after each); a 4 MB read file; sequential reads at 512 B–64 KB; 128 random reads (4 KB aligned, 4 KB unaligned,
64 B unaligned) on the 4 MB file and on the optional `file=`; 1,000 `lseek`s and 128 `lseek` + 1-byte reads with the
file opened `O_RDONLY` (fast-seek map) and `O_RDWR` (no map); 32 × (append 4 KB + `fsync`) and 32 × `fsync` with
nothing pending; 50 × `open` / `close` / `stat` / `stat` of a missing name; a directory of 128 empty files (create,
5 scans, stat each, 50 opens of a missing name, unlink each); cleanup check.

### 2.3 Safety and footprint

- The sd suite writes **only** below `/sd/.kiban_bench/`: `w.bin` (≤ 1 MB, deleted after each write test), `r.bin`
  (4 MB), `s.bin` (128 KB) and `d/` (128 empty files). It removes leftovers of an interrupted run first, deletes
  everything at the end and on every error path, and refuses to start with < 32 MB free. Peak card use ≈ 4.2 MB.
  `file=` is opened `O_RDONLY` only, and its path is not printed. The card is never formatted.
- Memory: at most ~6 MB of PSRAM (the 4 MB chase buffer is the largest) and internal buffers capped by the largest
  free internal block minus 24 KB.
- Run time of the measured run: 22.4 s for `all` (sd 9.2 s, chase 3.9 s, cpu 2.9 s, the others 0.2–1.7 s).

## 3. Running it

```
idf.py build                                         # in examples/bench
esptool --after watchdog-reset write-flash 0x40000 build/kiban_bench.bin
```

On the console:

```
mount 40000                       # the card at 40 MHz (boot mounts at 20 MHz)
bench                             # every suite (= bench all); or e.g. bench cpu chase, bench mem reps=15
bench sd file=/sd/some/large.file # adds random reads / opens on an existing file, read-only
bench sd "file=/sd/dir/A Book.epub"   # a path with spaces: quote the whole argument (or escape each space: \ )
```

Capture the console to a file and turn it into tables (paths relative to `examples/bench`):

```
python3 -I host/bench_report.py device.log [--host host.log] [--md out.md]
cc -O2 -Imain host/host_bench.c main/bench_core.c -o host_bench && ./host_bench all reps=15 > host.log
```

## 4. Results

All rows `[measured]`, one run, `reps=7`, medians (min and median within 2 % on every line below; spread ≤ 2 % except
where noted). SD rows are `esp_timer` times, not cycles. Cycles at 240 MHz (1 cycle = 4.17 ns).

### 4.0 Environment (`phase=env`)

cpu 240 MHz (measured 240.0), core 1, `-O2`, IDF v6.0.2; PSRAM octal 80 MHz, 8,388,608 B; D-cache 32 KB / 32 B / 8-way;
I-cache 16 KB / 32 B / 8-way; flash configured `dio` 80m, **in effect `qio`** (`spi0_ctrl=012c2008`,
`spi0_clock=00010001`); SD 40,000 kHz. Free memory belongs to the measuring app: internal 331,319 B free (largest
block 245,760), PSRAM 8,358,556 B free (largest 8,257,536).

### 4.1 CPU

| Kernel | Device cyc / op | Device ns | Host ns | Device / host |
|---|---|---|---|---|
| loop_empty | 4.00 | 16.7 | 1.10 | 15 |
| alu_dep / alu_indep4 (per step) | 4.00 / 4.00 | 16.7 / 16.7 | 2.34 / 0.61 | 7 / 28 |
| mul32_dep | 4.25 | 17.7 | 1.50 | 12 |
| div32_dep | 12.5 | 52.1 | 3.43 | 15 |
| mul64_dep | 13.5 | 56.3 | 1.00 | 56 |
| div64_dep (ROM call) | 139 | 580 | 3.73 | 155 |
| branch random / taken / alternate (per byte) | 13.0 / 14.0 / 13.0 | 54 / 58 / 54 | 0.94 / 0.60 / 0.90 | 58 / 97 / 60 |
| float add / mul / madd | 4.00 / 4.00 / 4.00 | 16.7 | 0.90 / 1.20 / 1.20 | 19 / 14 / 14 |
| float div (ROM call) | 58.8 | 245 | 3.30 | 74 |
| double add / mul / madd (soft) | 51.5 / 99.0 / 148 | 215 / 412 / 617 | 0.90 / 1.20 / 1.20 | 239 / 345 / 515 |
| double div (soft) | 609 | 2,539 | 3.88 | 654 |
| call inline / direct / indirect | 6.00 / 16.0 / 13.0 | 25 / 67 / 54 | 0.30 / 0.90 / 0.90 | 83 / 74 / 60 |
| call_recursive per level, depth 1 / 4 / 8 / 12 / 16 / 32 | 17.0 / 15.2 / 22.4 / 30.6 / 34.9 / 41.8 | 71 / 63 / 94 / 128 / 146 / 174 | 1.20 / 0.84 / 0.74 / 0.70 / 0.67 / 0.84 | 59 / 75 / 127 / 184 / 216 / 208 |
| strcmp_matrix (per compare) | 33.8 | 141 | 2.01 | 70 |
| strcmp_equal | 74.7 | 311 | 4.00 | 78 |
| memcmp 8 / 16 B equal | 67.4 / 89.4 | 281 / 373 | 2.36 / 1.76 | 119 / 212 |
| strlen / fnv1a (per name) | 52.4 / 40.1 | 218 / 167 | 1.99 / 2.03 | 110 / 82 |
| utf8_decode per byte: int 8 KB / PSRAM 8 KB hot / PSRAM 256 KB stream | 11.6 / 11.6 / 14.2 | 48 / 48 / 59 | 0.42 / 0.41 / 0.64 | 114 / 119 / 93 |

The string kernels gave identical cycles from internal RAM and from (cache-resident) PSRAM. UTF-8 decoding is
≈ 31 cycles per character (2.7 B / char) hot; 20.7 MB/s hot, 16.9 MB/s streaming from PSRAM. All self-checks `ok=1`.

### 4.2 Memory bandwidth (MB/s)

| Class | read | write | memset | memcpy |
|---|---|---|---|---|
| int (32 KB) | 426 | 767 | 765 | int → int 383 |
| psram_hot8k (cache-resident) | 426 (spread 8.7 %) | 763 | 754 | hot 4 KB → hot 4 KB 377 |
| psram (1 MB) | 58.2 | 35.1 | 35.1 | PSRAM → PSRAM 22.5 |
| rodata (128 KB flash) | 27.2 (spread 8.3 %) | — | — | rodata → int 27.2 |
| across classes | | | | PSRAM → int 59.5; int → PSRAM 35.1 |

### 4.3 Latency (cycles per hop, random pointer chase)

| Working set | 1 K | 8 K | 16 K | 24 K | 32 K | 48 K | 64 K | 128 K | 256 K | 1 M | 4 M |
|---|---|---|---|---|---|---|---|---|---|---|---|
| int | 2 | 2 | 2 | — | 2 | — | 2 | 2 | — | — | — |
| psram | 2 | 2 | 2 | 2 | 2 | 108 | 123 | 123 | 123 | 123 | 123 |
| psram rmw | — | 6 | — | — | 6 | — | 219 | — | — | 219 | 219 |

PSRAM 2 KB, 4 KB, 512 KB and 2 MB: same as their neighbours (2 / 2 / 123 / 123). The 1 MB walk in address order
(`order=seq`) costs the same 123 cycles per hop as the random one.

### 4.4 Stride (cycles per load, cold data cache)

| Stride (B) | 4 | 8 | 16 | 32 | 64 | 128 | 256 | 512 |
|---|---|---|---|---|---|---|---|---|
| int | 7.00 | 7.01 | 7.01 | 7.03 | 7.05 | 7.11 | 7.22 | 7.44 |
| psram | 17.2 | 30.8 | 61.5 | 123 | 123 | 123 | 123 | 123 |
| rodata | 36.0 | 72.0 | 144 | 288 | 288 | 288 | 288 | 288 |

### 4.5 Struct copy (cycles per copy)

| | 64 B | 256 B | 1,024 B |
|---|---|---|---|
| int | 79.1 | 201 | 682 |
| psram hot | 79.1 (spread 63 %: first repetition cold) | 201 | 682 |
| psram cold | 684 | 2,736 | 10,944 |

### 4.6 Allocator (cycles per call or pair)

| | pair 16 / 64 / 256 / 1 K / 4 K B | batch malloc 64 / 1 K | free LIFO / FIFO (64 B) | holed heap pair 32 / 2 K |
|---|---|---|---|---|
| caps_int | 1,440 / 1,254 / 1,480 / 1,480 / 1,946 | 1,195 / 1,285 | 664 / 666 | 1,446 / 1,943 |
| caps_psram | 1,396 / 1,396 / 1,418 / 1,418 / 1,418 | 792 / 814 (spread 17–18 %) | 607 / 611 | 1,143 / 1,417 |
| libc_malloc | 1,457 / 1,271 / 1,497 / 1,497 / 1,963 | 1,208 / 1,299 | 670 / 673 | — |

`frag`: internal 368 blocks, free 329,843 → 278,703 holed (largest 245,760 → 155,648) → 329,843 after; PSRAM 2,048
blocks, free 8,358,556 → 8,081,056 (largest 8,257,536 → 7,733,248) → 8,358,556 after. Both `ok=1`, no allocation
failures.

### 4.7 Code placement (cycles per op)

| flash straight 4,096 ops (I-cache thrash) | flash loop 256 | IRAM loop 256 | one 256-op pass: flash cold I-cache / flash warm / IRAM after thrash |
|---|---|---|---|
| 52.3 | 3.01 (spread 6.4 %) | 3.01 | 53.5 / 3.82 / 4.68 |

### 4.8 SD card at 40 MHz (1-bit)

| Measurement | Result |
|---|---|
| Sequential read, internal buffer, MB/s at 512 B / 1 K / 2 K / 4 K / 8 K / 16 K / 32 K / 64 K | 1.90 / 2.55 / 3.24 / 3.97 / 4.42 / 4.72 / 4.84 / 4.84 (270 µs per 512 B call, 13.5 ms per 64 KB call) |
| Sequential read into a PSRAM buffer, 4 K / 64 K | 3.86 / 3.97 MB/s |
| Write (+ fsync), internal buffer, 512 B / 4 K / 16 K / 64 K | 0.76 / 3.08 / 4.27 / 4.72 MB/s (with the final fsync 0.75 / 3.05 / 4.22 / 4.66); 4 MB at 64 K: 4.68 |
| Write from a PSRAM buffer, 4 K / 64 K | 3.24 / 3.75 MB/s |
| Create (`open O_CREAT`) / final `fsync` / `close` of those files | 1.2–1.9 ms / 2.8–2.9 ms / 44–50 µs |
| `unlink` of a 256 KB / 1 MB file | 4.0 ms / 4.0–6.1 ms |
| Random read, 4 MB scratch file, med (p95) | 4 KB aligned 1.05 ms (1.05); 4 KB unaligned 1.48 ms (1.63); 64 B 0.28 ms (0.55); 4 KB aligned into PSRAM 1.07 ms (1.08) |
| Random read, a 21 MB EPUB on the card, med (p95) | 4 KB aligned 1.13 ms (1.33); 4 KB unaligned 1.72 ms (2.11); 64 B 0.34 ms (0.67) |
| `lseek` alone, `O_RDONLY` (fast seek) / `O_RDWR` | 275 / 388 µs per call |
| `lseek` + 1-byte read, `O_RDONLY` / `O_RDWR`, med (p95) | 292 µs (295) / 318 µs (602) |
| `fsync` after a 4 KB append, med (p95, max) | 0.71 ms (2.78, 7.45); the 4 KB `write` itself 1.28 ms (1.65) |
| `fsync` with nothing pending | 6 µs |
| `open` + `close`, 4 MB file `O_RDONLY` / `O_RDWR`; the 21 MB EPUB `O_RDONLY` | 1.77 ms / 1.16 ms; 2.87 ms (close 29–34 µs) |
| `stat` of the 4 MB file / of a missing name | 1.11 ms / 1.03 ms |
| Directory of 128 files: create each / full scan / stat each / open a missing name / unlink each (med) | 4.68 ms / 8.87 ms per scan / 2.44 ms / 3.40 ms / 2.95 ms |
| Cleanup | scratch directory gone, free space equal to before (`ok=1`) |

## 5. Interpretation `[inference]`

- **PSRAM miss ≈ 123 cycles (512 ns) per 32 B line** once the working set exceeds the 32 KB data cache (knee between
  32 KB and 48 KB; at 48 KB only ≈ 12 % of the hops still hit). Below it, PSRAM through the cache is as fast as
  internal SRAM (2 cycles per dependent load). A dirty eviction adds ≈ 96 cycles (`chase_rmw` 219). Walking lines in
  address order costs the same as random: no prefetch helps a dependent walk. Streaming bandwidth is that latency
  per line: 32 B / 123 cycles = 62 MB/s against 58 MB/s measured read; write-allocate + write-back gives 32 B / 219
  cycles = 35 MB/s, exactly the measured PSRAM write, memset and int→PSRAM memcpy.
- The **cold struct copy is fully explained by line misses**: 64 B = 2 source lines × 123 + 2 destination lines × 219
  = 684 cycles (measured 684); 1,024 B = 32 × (123 + 219) = 10,944 (measured 10,944).
- **Flash `.rodata` miss ≈ 288 cycles (1.2 µs) per 32 B line** (stride flat from 32 B), 27 MB/s streaming, even with
  SPI0 in QIO. **Code fetched from flash costs about the same per line**: the 24,005 B straight-line function takes
  52.3 × 4,096 = 214 k cycles per call, ≈ 286 cycles per 32 B line, derived from `madd_stream`, not measured per line.
- **Code that misses the I-cache runs ≈ 17× slower** than the same op stream warm or in IRAM (`madd_stream` 52.3 vs
  3.01 cycles per op); one pass of a 1.5 KB body is ≈ 14× slower cold than warm (53.5 vs 3.82). IRAM gains nothing
  over warm flash code; it gains only when the I-cache is thrashed (4.68 vs 53.5). Since both cores share the caches
  (§1), core 0 activity can evict a hot path on core 1.
- **Arithmetic**: integer and single-float ops are 4 cycles per dependent step (no ILP visible: `alu_indep4` = `alu_dep`);
  32-bit divide 12.5. Every `double` op is a soft-float ROM call (52–609 cycles, 240–650× the host), as are float
  divide (59 cycles, 74×) and 64-bit divide (139, 155×), against 7–15× for plain integer work.
- **Calls**: a call of a leaf costs ≈ 10 cycles over inline; the cost per level of a recursion rises from 15–17
  cycles (depth ≤ 4) to 22–42 (depth 8–32), consistent with register-window spills past ~8 levels.
- **Branches and short string / memory compares** are 60–230× the host: an in-order core without the host's branch
  prediction and wide issue.
- **Allocator**: a malloc + free pair is ≈ 1,250–1,950 cycles (5–8 µs) in every class; batch malloc from PSRAM is
  cheaper than from internal RAM (≈ 800 vs ≈ 1,200 cycles); fragmentation barely changes it.
- **SD** at 40 MHz 1-bit: sequential reads reach 4.84 MB/s from 32 KB blocks (the bus limit is 5 MB/s); 4 KB reads
  give 3.97 MB/s; reads into PSRAM lose 3–18 % (bounce through internal RAM). A random 4 KB read costs ≈ 1.05–1.13 ms
  aligned and ≈ 1.5–1.7 ms unaligned (the partial first and last sectors go through FatFs's sector buffer); even
  64 B costs ≈ 0.3 ms. `lseek` alone costs
  ≈ one sector read (275 µs), so FatFs appears to load the sector at the new position; the `O_RDWR` open (no fast-seek
  map) is slower per seek and has a long tail. Metadata is expensive: `open` 1.2–2.9 ms (a read-only open builds the
  fast-seek map: ≈ 0.6 ms more than `O_RDWR` for 4 MB, more for a larger file), `stat` ≈ 1.1 ms, directory operations 2.4–4.7 ms each in a 128-entry directory and
  ≈ 70 µs per entry for a scan. Writes need ≥ 16 KB blocks to approach 4.3–4.7 MB/s.

## 6. Limits

- One board, one card, one run, one temperature; the second core is idle but not stopped.
- Interrupts are masked only on core 1 and only inside timed repetitions; the allocator and SD suites run with
  interrupts on.
- The cycle counter is per core; all timed code runs on core 1 (`cpu_mhz_measured` checks its rate).
- `call_*` and `code` costs depend on code layout and the window state at entry; they are orders of magnitude, not
  exact constants (and `call_direct` differs between the measured image and `examples/bench`, see Provenance).
- PSRAM-hot lines include the first repetition's misses in the maximum; median and minimum are the steady state.
- `alloc frag` and `sd cleanup` `ok` compare exact free-byte counts; a concurrent allocation on core 0 or FAT
  bookkeeping can make them report 0 without a leak.
- SD figures depend on the card model and its state (erase blocks, cache, fragmentation, fill level).

## 7. Cache size and a reader workload

A reader application's "page made from a resume index" path (inflate an EPUB XHTML item, parse it with expat, cascade
CSS per element, break lines) on this board, 240 MHz, `-O2`, the same commands per build; the builds differ only in
sdkconfig, and every build's output equals the host's `[measured]`. Median ms per page:

| Workload | 16 KB I / 32 KB D (IDF default) | 32 KB I / 64 KB D | 32 / 64 KB + hot code in IRAM |
|---|---|---|---|
| A: long Japanese chapters, small stylesheet | 383 | 199 | 189 |
| B: technical book, 770-rule stylesheet | 409 | 215 | 200 |
| C: Chinese novel, 421 short items | 54 | 43 | 38 |
| D: Chinese novel, 1,527 short items | 49 | 36 | 31 |

- The bigger caches (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` with 8 ways and 32 B lines,
  `CONFIG_ESP32S3_DATA_CACHE_64KB`) take 48 KB of internal heap: 16 KB I-cache, 32 KB D-cache
  (`components/heap/port/esp32s3/memory_layout.c` `[source]`); internal free at idle 348 → 300 KB `[measured]`.
- Placing the hot path's code in IRAM (≈ 144 KB) on top of them saves ≈ 5 % (A), 7 % (B) and 13 % (D, small pages)
  `[measured]`; with the default caches IRAM placement alone gives about what the bigger caches give, for three
  times the RAM.
- The hot path's code (≈ 144 KB) is several times the 16 KB I-cache. With the default caches the median page of
  workload A moved by ≈ 18 % between two builds of the application whose measured path had the same logic
  `[measured]`; that the cause is code layout under I-cache misses (§5: ≈ 17× per missed op) is `[inference]`.

Recommendation `[inference]`: a product with large parsing or layout code paths (XML, CSS, text shaping) should use
32 KB I-cache and 64 KB D-cache, and keep IRAM placement for code that must run with the caches disabled or that
still dominates after measuring with the bigger caches.

## 8. Open items

- **PIE** (the S3's 128-bit SIMD instructions, TRM ch. 1) `[source: TRM]`: no help for branchy parsing. Candidates
  later: glyph blits, rotation, 2-bit → 1-bit conversion. No movemask; loads and stores need 16-byte alignment.
