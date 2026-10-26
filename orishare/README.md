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

Our benchmark now support the following hashmaps:
1. unordered_map
2. sharehash
3. mapembed
4. cuckoohash
5. sparsehash

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

The runner evaluates all five implementations at 100,000, 1,000,000 and 5,000,000
keys, with three repetitions each (45 separate processes). Each repetition uses
the same dataset seed for every implementation. It pins execution to one allowed
CPU and sets `OMP_NUM_THREADS=1` and `OMP_DYNAMIC=FALSE`. Output goes to a fresh
`result/single_thread_<UTC time>/` directory containing `benchmark_results.csv`,
`summary.csv` (completed-query medians), `memory_results.csv`, `memory_summary.csv`
(insertion-checkpoint medians), `run_manifest.json` (configuration, commands,
checksums and validation), and per-run logs. Existing result directories are not overwritten.
Use `--sizes`, `--repeats`, `--seed` or `--result-dir` to customize the suite.

The original initialization order, random uint64 key/value workload, hash
implementations, hash parameters, insertion/query order and throughput formula
are retained. The optional `--seed` makes the dataset reproducible; when omitted,
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
- `index_rss_bytes_estimate` and `rss_bpk_bytes_per_key` estimate the index's
  resident footprint using `/proc/self/smaps_rollup`. Untouched reserved pages
  do not contribute in full. Raw `process_rss_bytes` and
  `peak_process_rss_bytes` include the input dataset and process/runtime overhead;
  they are not index-only memory or index-only peak memory.

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
