# 六类哈希单线程评测：ShareHash 扣除锁对象内存

已执行 6 类哈希 × 3 个规模 × 3 次重复，共 **54 次独立进程运行，40 次通过，14 次失败**。单表 ShareHash、unordered_map、CuckooHash、SparseHash 各 9/9 通过；子表 ShareHash 4/9 通过，MapEmbed 0/9 通过。所有计划任务均已完成，无超时或中断。

“单表 ShareHash”指当前已经修复漏查的 `include/ShareHash.h`，不是历史漏查版。子表版本使用 `include/ShareHash_subtable.h`。本轮只接入独立适配器和测量字段，两版哈希算法头文件 SHA256 均与运行前一致。

## 旧结果是否包含锁，以及本次扣除口径

**旧五类哈希基准统计了 ShareHash 锁内存。** 原 `memoryUsage()` 显式计入 `item_num * sizeof(SpinLock)`，`sizeof(*this)` 还包含 1 个表级锁；分配器与 RSS 实测同样包含锁存储。本次将锁对象本体单列，从 ShareHash 的主内存/BPK 比较指标中扣除：

```text
单表扣除量 = (槽容量 + 1 个表级锁) × sizeof(SpinLock)
子表扣除量 = (总槽容量 + T 个表级锁) × sizeof(SpinLock)
扣锁 reported bytes = reported_memory_bytes - lock_memory_bytes
扣锁 heap bytes = index_heap_bytes_estimate - lock_memory_bytes
扣锁 BPK = 对应扣锁 bytes / 请求插入条数
```

本机 `sizeof(SpinLock)=1` byte。扣除仅针对两种 ShareHash；其他四类沿用原测量，扣除列为 0 不代表它们没有内部锁。锁指针、结构体 padding、分配器元数据仍保留，因此这些是**扣除锁对象字节后的估计**。实际锁仍分配并执行，吞吐量包含加解锁时间；RSS 与峰值 RSS 保持实测，没有伪造“扣锁 RSS”。旧结果目录不修改。

原始含锁列继续保存以供追溯。后续比较应使用新增 `heap_bpk_excluding_locks_bytes_per_key`，并结合正确性状态。各实现原始 `memory_consumption()` 的覆盖范围不同，尤其 unordered_map 仅报 56 bytes，不能据其原始 BPK 排名。

## 运行配置

- 数据规模 100,000 / 1,000,000 / 5,000,000；每类每规模重复 3 次。
- CPU 0，单线程 `-p 1`；`OMP_NUM_THREADS=1`、`OMP_DYNAMIC=FALSE`；任务依次执行。
- 种子 20261025 / 20261026 / 20261027，相同规模与重复使用相同随机 uint64 键值对，键等于值，载荷 16 bytes/条，未去重。
- 两种 ShareHash 初始均为总计 5,000 桶、20,000 槽。子表版 T=16、投影负载阈值 0.8。
- 子表版还使用独立位路由、SplitMix64 混合及局部增长策略；这是两个现有版本的整体比较，不能把差异单独归因于 T。
- Intel(R) Xeon(R) Gold 5320 CPU @ 2.20GHz；glibc 2.31；编译保留 `-std=gnu++14 -pg -g -O1 -fopenmp`，ASan/TSan 关闭。
- 时间：2026-10-27 12:49:02 至 2026-10-27 12:50:02（北京时间，UTC+8）。
- 基准检查插入返回成功数和查询命中数是否等于请求条数；不是全量返回值校验。下方故障诊断另外核对了两个种子前 100 万条的完整值。

## 有效样本的插入后内存

本表只使用进程、插入计数、查询计数和 CSV 检查全部通过的样本，来源为 `valid_only_memory_summary.csv`。所有 BPK 均为 **bytes/key**，包含 KV 载荷；MiB=2^20 bytes。子表版各组只有 2 / 1 / 1 个有效样本，不能称作完整三次有效重复。MapEmbed 无有效样本，空值不是 0。

heap 与 RSS 原始索引估计沿用 `(after_init-before_init)+(after_insert-before_insert)` 排除实测输入生成占用。表中的 heap 再扣除锁对象字节；RSS 仍包含锁。

| 条数 | 哈希 | 有效样本 | 扣锁分配器 MiB | 扣锁分配器 BPK | 实测 RSS BPK（含锁） |
|---:|---|---:|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 2.924 | 30.658 | 38.011 |
| 100,000 | ShareHash（子表） | 2/3 | 3.514 | 36.846 | 47.370 |
| 100,000 | unordered_map | 3/3 | 4.375 | 45.876 | 47.800 |
| 100,000 | MapEmbed | 0/3 | — | — | — |
| 100,000 | CuckooHash | 3/3 | 8.509 | 89.228 | 91.177 |
| 100,000 | SparseHash | 3/3 | 1.826 | 19.146 | 22.610 |
| 1,000,000 | ShareHash（单表） | 3/3 | 23.352 | 24.486 | 29.151 |
| 1,000,000 | ShareHash（子表） | 1/3 | 29.250 | 30.671 | 36.164 |
| 1,000,000 | unordered_map | 3/3 | 41.564 | 43.583 | 43.774 |
| 1,000,000 | MapEmbed | 0/3 | — | — | — |
| 1,000,000 | CuckooHash | 3/3 | 22.009 | 23.079 | 23.278 |
| 1,000,000 | SparseHash | 3/3 | 16.892 | 17.712 | 18.862 |
| 5,000,000 | ShareHash（单表） | 3/3 | 186.777 | 39.170 | 46.259 |
| 5,000,000 | ShareHash（子表） | 1/3 | 186.843 | 39.184 | 43.667 |
| 5,000,000 | unordered_map | 3/3 | 198.118 | 41.548 | 41.587 |
| 5,000,000 | MapEmbed | 0/3 | — | — | — |
| 5,000,000 | CuckooHash | 3/3 | 148.009 | 31.040 | 31.079 |
| 5,000,000 | SparseHash | 3/3 | 82.161 | 17.231 | 18.016 |

## 有效样本的吞吐量

单位 Mops/s；来源 `valid_only_summary.csv`，只包含通过样本。实际锁操作保留。

| 条数 | 哈希 | 有效样本 | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 3.393 | 10.532 |
| 100,000 | ShareHash（子表） | 2/3 | 4.516 | 13.415 |
| 100,000 | unordered_map | 3/3 | 6.917 | 27.171 |
| 100,000 | MapEmbed | 0/3 | — | — |
| 100,000 | CuckooHash | 3/3 | 6.878 | 11.073 |
| 100,000 | SparseHash | 3/3 | 2.833 | 11.941 |
| 1,000,000 | ShareHash（单表） | 3/3 | 4.107 | 8.403 |
| 1,000,000 | ShareHash（子表） | 1/3 | 4.113 | 9.713 |
| 1,000,000 | unordered_map | 3/3 | 5.297 | 16.911 |
| 1,000,000 | MapEmbed | 0/3 | — | — |
| 1,000,000 | CuckooHash | 3/3 | 2.734 | 9.929 |
| 1,000,000 | SparseHash | 3/3 | 2.309 | 11.566 |
| 5,000,000 | ShareHash（单表） | 3/3 | 2.860 | 5.149 |
| 5,000,000 | ShareHash（子表） | 1/3 | 3.032 | 6.474 |
| 5,000,000 | unordered_map | 3/3 | 3.218 | 9.577 |
| 5,000,000 | MapEmbed | 0/3 | — | — |
| 5,000,000 | CuckooHash | 3/3 | 2.005 | 5.414 |
| 5,000,000 | SparseHash | 3/3 | 2.144 | 6.260 |

## 两种 ShareHash 使用相同有效种子的对比

为了避免样本种子不同造成误解，本表仅比较两版都通过的种子交集；详细性能也保存在 `sharehash_matched_comparison.csv`。

| 条数 | 配对数 | 种子 | 单表扣锁 heap BPK | 子表扣锁 heap BPK | 子表变化 |
|---:|---:|---|---:|---:|---:|
| 100,000 | 2 | 20261025;20261027 | 30.658 | 36.846 | +20.18% |
| 1,000,000 | 1 | 20261025 | 24.486 | 30.671 | +25.26% |
| 5,000,000 | 1 | 20261025 | 39.170 | 39.184 | +0.04% |

本轮有效配对样本中，子表版在 10 万和 100 万条下没有降低扣锁 BPK，500 万条时两者接近。有效样本不足 3 次，且子表版本存在插入失败，不能据此宣称稳定的空间或性能优势。

## 子表 ShareHash 插入失败

子表版 9 次进程均正常退出，但 5 次插入成功数低于请求条数。每次查询命中数都等于成功插入数：这是插入返回 `false`，不能表述为已成功插入键的漏查。

| 请求条数 | 种子 | 插入成功数 | 查询命中数 | 未插入条数 |
|---:|---:|---:|---:|---:|
| 100,000 | 20261026 | 99,999 | 99,999 | 1 |
| 1,000,000 | 20261026 | 999,998 | 999,998 | 2 |
| 1,000,000 | 20261027 | 999,999 | 999,999 | 1 |
| 5,000,000 | 20261026 | 4,999,998 | 4,999,998 | 2 |
| 5,000,000 | 20261027 | 4,999,999 | 4,999,999 | 1 |

额外诊断复现出三个不同的失败位置（下标从 0 开始）：

| 种子 | 插入下标 | key | 子表 | 桶数：扩容前→后 | home：前→后 |
|---:|---:|---:|---:|---|---|
| 20261026 | 20626 | 6162235414312297564 | 5 | 626→1252 | 625→1251 |
| 20261026 | 425422 | 5779845426815171529 | 6 | 10016→20032 | 10015→20031 |
| 20261027 | 232063 | 3434778668289550253 | 4 | 5008→10016 | 5007→10015 |

三者扩容前后均落在末桶，且该桶均占用 4/4，没有右邻桶可借位。`resize_locked()` 本身成功并完整迁移，随后 [insert()](../../include/ShareHash_subtable.h) 只再调用一次 `insert_without_growth()`；该次仍失败就返回 false。这是当前重试上限与右边界规则共同造成的限制，本轮保留该算法版本及失败结果。

诊断在原头文件和仅用于诊断的插桩副本上交叉复现。对 seed=20261026/20261027 的前 100 万条，成功插入的全部记录均可查询且值正确：分别成功 999,998 / 999,999 条，成功键漏查 0、错误值 0，失败键均未入表。诊断不计入吞吐量样本，日志见 [unmodified.log](diagnostics/unmodified.log)、[instrumented.log](diagnostics/instrumented.log)。

## MapEmbed 与原始汇总

- MapEmbed 9/9 次收到 SIGABRT，退出码 -6，stderr 均有 `stack smashing detected`，与原有越界问题一致。10 万与 100 万条的 6 次有插入后快照；500 万条的 3 次没有。
- `benchmark_results.csv` 的 45 条完成查询记录包含 5 条失败的子表 ShareHash。`memory_results.csv` 的 51 条快照还包含 6 条失败 MapEmbed。
- runner 沿用原有汇总语义：`summary.csv` 和 `memory_summary.csv` 保留失败数值作诊断，必须结合通过数。主报告使用另行生成的 `valid_only_*`，不会用少存键的样本参与有效比较。
- runner 总状态 `failed`、退出码 1，反映 14 次失败；不是任务中断。

## 文件与复现

- [valid_only_memory_summary.csv](valid_only_memory_summary.csv)：只含通过样本的插入后内存/BPK汇总。
- [valid_only_summary.csv](valid_only_summary.csv)：只含通过样本的吞吐量及其他指标汇总。
- [sharehash_matched_comparison.csv](sharehash_matched_comparison.csv)：相同有效种子交集下的两版对比。
- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：所有取得的原始记录，保留失败。
- [summary.csv](summary.csv)、[memory_summary.csv](memory_summary.csv)：runner 的完整诊断汇总。
- [run_manifest.json](run_manifest.json)：54 次命令、退出码、配置、源码/二进制/四份原始CSV校验和。
- [validation.json](validation.json)：扣锁公式、原始/有效汇总、配对比较及产物校验结果。
- [diagnostics/](diagnostics/)：故障复现程序、插桩副本和两份日志；生产算法头文件未改。
- [logs/](logs/)：逐次 stdout/stderr 及 gprof 产物。

在项目根执行以下命令会创建新目录，默认运行六类索引：

```sh
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```

用未修改的生产头复现插入失败及完整值核验：

```sh
c++ -std=c++14 -O1 -pthread -Iinclude -DUNMODIFIED_HEADER \
  result/six_hashes_excluding_locks_20261027/diagnostics/sharehash_subtable_diagnostic.cpp \
  -o /tmp/sharehash_subtable_diagnostic
/tmp/sharehash_subtable_diagnostic 1000000
```
