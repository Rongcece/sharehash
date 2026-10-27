# sharehash

## Description


## Dependencies

```
sudo apt update
sudo apt install build-essential libboost-all-dev libsparsehash-dev
```
We make and install cuckoo hashmap from [libcuckoo](https://github.com/efficient/libcuckoo)

## Build and Run

```
mkdir build && cd build
cmake ..
make -j8
./benchmark -t $(hashmap_type) -d $(dataset_number)
```

The default benchmark suite compares these six hashmaps:
1. unordered_map
2. sharehash
3. cuckoohash
4. sparsehash
5. sharehash_subtable (independent subtable variant)
6. sharehash_pagepool (shared page-pool variant)

## CSV results and single-thread evaluation

Each successful benchmark invocation appends one row to
`result/benchmark_results.csv` in the project directory. The original console
throughput and space output is retained. `--result-dir` selects another directory;
directories are created automatically and the CSV header is written once.
Immediately after insertion, an additional `after_insert` checkpoint is flushed
to `result/memory_results.csv`. It preserves memory/BPK and insertion throughput
even if an existing hashmap later aborts during queries. It does not imply that
the query phase succeeded.

Build a separate binary without sanitizers, retaining the project's existing
`-pg -g -O1` compilation options:

```sh
cmake -S . -B build-single-thread -DUSE_ASAN=OFF -DUSE_TSAN=OFF
cmake --build build-single-thread --target benchmark -j4
./build-single-thread/benchmark -t sharehash -d 1000000 -p 1 --seed 20261025
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```

The runner evaluates six implementations (excluding MapEmbed and SpaceLimitA) at 100,000, 1,000,000 and 5,000,000
keys, with three repetitions each (54 separate processes). Each repetition uses
the same dataset seed for every implementation. It pins execution to one allowed
CPU and sets `OMP_NUM_THREADS=1` and `OMP_DYNAMIC=FALSE`. Output goes to a fresh
`result/single_thread_<UTC time>/` directory containing `benchmark_results.csv`,
`summary.csv` (completed-query medians), `memory_results.csv`, `memory_summary.csv`
(insertion-checkpoint medians), `run_manifest.json` (configuration, commands,
checksums and validation), and per-run logs. Existing result directories are not overwritten.
Use `--sizes`, `--repeats`, `--seed` or `--result-dir` to customize the suite.

For the original five implementations, initialization order, random uint64
key/value workload, hash parameters, insertion/query order and throughput formula
are retained. The separate `sharehash_subtable` adapter uses the current subtable
implementation unchanged: automatic T=16, maximum load factor 0.8, and the same
20,000 initial total slots as the single-table ShareHash. The optional `--seed` makes the dataset reproducible; when omitted,
the original random-device seeding is used. The two loops additionally count
successful insert returns and query hits. The runner checks both against the
requested size; it does not validate returned values (the existing MapEmbed
adapter only reports presence). The existing `-i`/`-r` options and multithread
range partitioning are unchanged; this suite exclusively uses `-p 1`.
Throughput includes the small additional return-value counting overhead.

### Memory and BPK definitions

**BPK means bytes per key**, including stored keys and values, rather than bits
per key or extra bytes beyond the 16-byte key/value payload. All BPK fields divide
by `dataset_size`. Rows with failed insert/hit checks must not be interpreted as
valid full-dataset memory comparisons.

- `reported_memory_bytes` and `reported_bpk_bytes_per_key` preserve each adapter's
  original `memory_consumption()` estimate. These estimates have different
  coverage and should not be used alone to rank the implementations:
  unordered_map counts only the container object (56 bytes here); CuckooHash counts
  slot payload capacity; SparseHash estimates payload capacity; ShareHash counts
  its object, bucket metadata, slots, per-slot locks and bitmap; MapEmbed includes
  fixed preallocated structures and an unused inline 480,000,160-byte KV array.
- `index_heap_bytes_estimate` and `heap_bpk_bytes_per_key` use glibc live heap
  allocations (`uordblks + hblkhd`, including mmap allocations). They include
  allocator block rounding and reserved arrays, including untouched pages.
  glibc >= 2.33 uses `mallinfo2`; older versions use `mallinfo` and reject negative
  counters caused by signed 32-bit overflow.
- New runs use `reported_memory_excluding_locks_bytes` /
  `reported_bpk_excluding_locks_bytes_per_key` and
  `index_heap_excluding_locks_bytes_estimate` /
  `heap_bpk_excluding_locks_bytes_per_key` for comparisons excluding ShareHash
  lock-object storage. Each is the corresponding raw byte count minus
  `lock_memory_bytes`, then divided by the requested key count for BPK. Single-table
  ShareHash subtracts `(slot_capacity + 1) * sizeof(SpinLock)`; the subtable version
  subtracts `(total_slot_capacity + T) * sizeof(SpinLock)`. The V2-A variant
  subtracts `(total_slot_capacity + T + 1) * sizeof(SpinLock)` to include its
  additional global budget lock. The table locks embedded
  in their objects are included. Padding, pointer fields and allocator metadata
  remain. Both original raw fields and the deduction are retained for auditing.
  Other implementations report zero deduction; this does not assert that their
  implementations contain no locks. The deduction happens outside timed loops;
  all actual locks still execute, so throughput includes locking costs.
- `index_rss_bytes_estimate` and `rss_bpk_bytes_per_key` estimate the index's
  resident footprint using `/proc/self/smaps_rollup`. Untouched reserved pages
  do not contribute in full. Raw `process_rss_bytes` and
  `peak_process_rss_bytes` include the input dataset and process/runtime overhead;
  they are not index-only memory or index-only peak memory. RSS remains an actual
  measurement with locks present: page residency cannot be converted to a true
  lock-free RSS by subtracting object byte counts.

Memory snapshots are taken before index initialization, after initialization,
after generating the dataset (before insertion), after insertion, and after the workload. All
sampling and CSV writes occur outside the throughput timing intervals. For
both heap and RSS, the index estimate is:

```text
(after_init - before_init) + (after_workload - before_insert)
```

This subtracts the measured input-generation footprint while preserving index
initialization allocations. CSV also saves the raw snapshots, input memory
estimates and `dataset_payload_bytes` for auditing. These are estimates rather
than exact allocation ownership: lazy OpenMP/runtime allocations, allocator
reuse, temporary buffers and retained freed pages may affect the numbers. The
current MapEmbed configuration has 500,000 buckets with 16 slots each, no automatic
capacity extension in this benchmark, and substantial fixed memory even at small
dataset sizes. The Linux/glibc sampling APIs are required for these measurements.
For `memory_results.csv`, substitute `after_insert` for `after_workload` in the
formula. Its snapshots precede CSV-writing allocations. Completed-query snapshots
may additionally include allocations retained from writing the insertion checkpoint.

### Recorded baseline

The 2026-10-25 single-thread suite is documented in
[result/single_thread_20261025/RESULTS.md](result/single_thread_20261025/RESULTS.md).
All 45 jobs were attempted; 27 passed return-count/hit checks. ShareHash had missed
queries in all nine runs, and MapEmbed aborted with stack corruption in all nine
runs (at 5 million keys, before the insertion checkpoint). These existing hashmap
issues were preserved. The runner exits nonzero when any job fails; failure logs
and available memory checkpoints remain saved. Inspect validity counts before
using summary metrics as comparisons.

The six-index run with both ShareHash variants and lock-object memory deduction is
recorded in [result/six_hashes_excluding_locks_20261027/RESULTS.md](result/six_hashes_excluding_locks_20261027/RESULTS.md).
It attempted all 54 jobs: 40 passed, five subtable ShareHash runs had explicit
insertion failures, and nine MapEmbed runs aborted. The report provides separate
`valid_only_*` CSVs and a comparison matched by valid seed. The runner's standard
summary files retain failed numeric samples for diagnostics; always inspect their
pass counts. A completed process is not sufficient to establish a valid index.

### Shared page-pool benchmark competitor

`-t sharehash_pagepool` selects `include/ShareHash_pagepoolB.h` through a
benchmark-only type-name isolation adapter. The algorithm header is unchanged.
The default suite compares single-table, subtable, page-pool ShareHash,
unordered_map, CuckooHash and SparseHash. MapEmbed and SpaceLimitA are excluded.
PagePool B uses default parameters (stable/migration factors both 48 bytes/key) and emits `space_budget_results.csv` for
configuration, rejection and requested-byte ledger diagnostics. The default
physical cap is size_t max; affine admission remains enabled. Lock deduction
is `(physical slot capacity + T + 1) * sizeof(SpinLock)`.

The shared page-pool run is recorded in [result/six_hashes_pagepool_20261027/RESULTS.md](result/six_hashes_pagepool_20261027/RESULTS.md).
