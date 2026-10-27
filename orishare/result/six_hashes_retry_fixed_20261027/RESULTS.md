# 六类哈希单线程评测：子表 ShareHash 插入重试修复后

已完成 6 类哈希 × 3 个规模 × 3 次重复，共 **54 次独立进程运行，45 次通过，9 次失败**。修复后的子表 ShareHash **9/9 次通过**，全部插入成功数与查询命中数等于请求条数。单表 ShareHash、unordered_map、CuckooHash、SparseHash 也各 9/9 次通过。9 次失败全部来自原有 MapEmbed 栈破坏。

相对上一轮六类评测，登记源码中只有 `include/ShareHash_subtable.h` 发生变化：插入时因借位失败最多额外扩容当前子表 4 次，每次重新尝试，替代原来的一次扩容重试。负载阈值检查、哈希路由、T、右借位及其他五类实现保持原版本；编译参数相同。本轮未修改算法或测评代码。

## 运行配置

- 条数：100,000 / 1,000,000 / 5,000,000；每类每规模 3 次。
- 种子：20261025 / 20261026 / 20261027；相同规模与重复的六类哈希使用同一数据。随机 uint64 键值对，键等于值，未去重，每条载荷 16 bytes。
- 单线程 `-p 1`，绑定 CPU 0；`OMP_NUM_THREADS=1`、`OMP_DYNAMIC=FALSE`；任务顺序执行。
- 两种 ShareHash 初始总计 5,000 桶、20,000 槽。子表版 T=16，投影负载阈值 0.8，借位失败后的局部扩容预算为每次写入 4 次。
- CPU：Intel(R) Xeon(R) Gold 5320 CPU @ 2.20GHz；glibc 2.31。
- 编译选项保留 `-std=gnu++14 -pg -g -O1 -fopenmp`，ASan/TSan 关闭。
- 运行时间：2026-10-27 13:17:13 至 2026-10-27 13:18:13（北京时间，UTC+8）。
- 正确性范围：基准检查插入返回成功数与查询命中数，不对每次返回值内容做完整校验；此前修复回归另行验证了返回值。

## 内存与锁口径

延续上一轮要求，主内存/BPK列扣除两种 ShareHash 的锁对象字节，原始含锁字段仍保留。所有 BPK 为 **bytes/key**，包括 KV 载荷；MiB=2^20 bytes。

```text
单表扣除量 = (总槽容量 + 1) × sizeof(SpinLock)
子表扣除量 = (总槽容量 + T) × sizeof(SpinLock)
索引 heap 原始估计 = (after_init-before_init) + (after_insert-before_insert)
扣锁 heap 估计 = 索引 heap 原始估计 - lock_memory_bytes
扣锁 BPK = 扣锁 bytes / 请求插入条数
```

本机 SpinLock 为 1 byte。扣减不包含锁指针、结构体 padding 或分配器元数据，也不是取消锁后的实际运行。实际锁照常执行，因此吞吐量仍包含加解锁时间；RSS 仍为含锁实测。其他四类索引不做锁扣除，扣除项为 0 不代表它们没有锁。各类原始 `memory_consumption()` 覆盖范围不同，尤其 unordered_map 仅报 56 bytes，跨实现比较主要参考分配器估计。

## 插入后内存：仅使用通过样本

取 `valid_only_memory_summary.csv` 的三次中位数。除 MapEmbed 外，每组均有完整 3 次有效样本。MapEmbed 的失败快照仅保留在原始数据与诊断汇总中，不参与下表；空值表示没有有效结果。

| 条数 | 哈希 | 通过 | 扣锁分配器 MiB | 扣锁分配器 BPK | 实测 RSS BPK（含锁） |
|---:|---|---:|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 2.924 | 30.658 | 38.011 |
| 100,000 | ShareHash（修复后子表） | 3/3 | 3.343 | 35.054 | 43.377 |
| 100,000 | unordered_map | 3/3 | 4.375 | 45.876 | 47.800 |
| 100,000 | MapEmbed | 0/3 | — | — | — |
| 100,000 | CuckooHash | 3/3 | 8.509 | 89.228 | 91.177 |
| 100,000 | SparseHash | 3/3 | 1.826 | 19.146 | 22.487 |
| 1,000,000 | ShareHash（单表） | 3/3 | 23.352 | 24.486 | 29.147 |
| 1,000,000 | ShareHash（修复后子表） | 3/3 | 32.141 | 33.702 | 38.007 |
| 1,000,000 | unordered_map | 3/3 | 41.564 | 43.583 | 43.774 |
| 1,000,000 | MapEmbed | 0/3 | — | — | — |
| 1,000,000 | CuckooHash | 3/3 | 22.009 | 23.079 | 23.273 |
| 1,000,000 | SparseHash | 3/3 | 16.892 | 17.712 | 18.862 |
| 5,000,000 | ShareHash（单表） | 3/3 | 186.777 | 39.170 | 46.258 |
| 5,000,000 | ShareHash（修复后子表） | 3/3 | 186.843 | 39.184 | 43.667 |
| 5,000,000 | unordered_map | 3/3 | 198.118 | 41.548 | 41.587 |
| 5,000,000 | MapEmbed | 0/3 | — | — | — |
| 5,000,000 | CuckooHash | 3/3 | 148.009 | 31.040 | 31.079 |
| 5,000,000 | SparseHash | 3/3 | 82.161 | 17.231 | 18.015 |

## 插入与查询吞吐量

取 `valid_only_summary.csv` 的三次中位数，单位 Mops/s（百万次操作/秒），计时区间沿用原基准。

| 条数 | 哈希 | 通过 | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 3.417 | 10.649 |
| 100,000 | ShareHash（修复后子表） | 3/3 | 4.566 | 13.365 |
| 100,000 | unordered_map | 3/3 | 6.580 | 27.108 |
| 100,000 | MapEmbed | 0/3 | — | — |
| 100,000 | CuckooHash | 3/3 | 6.894 | 11.024 |
| 100,000 | SparseHash | 3/3 | 2.829 | 11.969 |
| 1,000,000 | ShareHash（单表） | 3/3 | 4.113 | 8.386 |
| 1,000,000 | ShareHash（修复后子表） | 3/3 | 3.961 | 9.721 |
| 1,000,000 | unordered_map | 3/3 | 5.298 | 16.886 |
| 1,000,000 | MapEmbed | 0/3 | — | — |
| 1,000,000 | CuckooHash | 3/3 | 2.737 | 9.923 |
| 1,000,000 | SparseHash | 3/3 | 2.311 | 11.524 |
| 5,000,000 | ShareHash（单表） | 3/3 | 2.860 | 5.173 |
| 5,000,000 | ShareHash（修复后子表） | 3/3 | 3.075 | 6.493 |
| 5,000,000 | unordered_map | 3/3 | 3.234 | 9.543 |
| 5,000,000 | MapEmbed | 0/3 | — | — |
| 5,000,000 | CuckooHash | 3/3 | 2.016 | 5.411 |
| 5,000,000 | SparseHash | 3/3 | 2.147 | 6.349 |

## 两种 ShareHash 对比

三组均使用相同的三个有效种子。扣锁分配器 BPK 对比如下；对应吞吐量见上表及 `sharehash_matched_comparison.csv`。

| 条数 | 配对数 | 单表 BPK | 修复后子表 BPK | 子表变化 |
|---:|---:|---:|---:|---:|
| 100,000 | 3 | 30.658 | 35.054 | +14.34% |
| 1,000,000 | 3 | 24.486 | 33.702 | +37.64% |
| 5,000,000 | 3 | 39.170 | 39.184 | +0.04% |

本轮子表版不再出现插入返回失败，但没有降低扣锁分配器 BPK：10 万与 100 万条高于单表版，500 万条接近。子表版查询吞吐量在本轮三个规模均较高，插入吞吐量则在 100 万条略低。两个版本同时存在哈希混合、负载阈值和增长策略差异，这些是版本整体测量，不能单独归因于子表数量。

## 失败与版本说明

- MapEmbed 9 次均因 SIGABRT 退出，退出码 -6，stderr 包含 `stack smashing detected`。10 万与 100 万条的 6 次在保存插入快照后退出；500 万条的 3 次没有完成插入快照。
- `benchmark_results.csv` 有 45 条通过记录；`memory_results.csv` 有 51 条快照，其中 6 条来自失败 MapEmbed。
- runner 的总状态为 `failed`、退出码为 1，准确反映 MapEmbed 的 9 次失败。54 次计划运行均已执行，无超时或中断。
- 上一轮子表版只有 4/9 次通过，本轮为 9/9。旧结果保留在 [上一轮六类评测](../six_hashes_excluding_locks_20261027/RESULTS.md)，其失败样本不能当作修复后的结果。
- 四次局部扩容预算仍是有限策略：病态碰撞或迁移失败仍可能返回 false。本轮三个规模及三个种子通过，不表示任意键分布都必然插入成功。

源码校验和：

- 单表 ShareHash：`9d4508ca98816a566521a4186c75dbcebaa35c81723d3f2cabfe697f5c49fe85`。
- 修复后子表 ShareHash：`9c14b2f5686ed3ff02f6b2f76ab75bb8d82ae66d8cc31647de50cfaf06d091f1`。

## 结果文件与核对

- [valid_only_memory_summary.csv](valid_only_memory_summary.csv)：有效插入后内存与扣锁BPK汇总。
- [valid_only_summary.csv](valid_only_summary.csv)：有效吞吐量及其他指标汇总。
- [sharehash_matched_comparison.csv](sharehash_matched_comparison.csv)：三个相同有效种子下的两版对比。
- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：原始记录。
- [summary.csv](summary.csv)、[memory_summary.csv](memory_summary.csv)：runner 标准汇总，后者含失败 MapEmbed 诊断样本。
- [run_manifest.json](run_manifest.json)：全部命令、退出码、配置、源码/二进制/原始CSV校验和。
- [validation.json](validation.json)：原始/扣锁公式、通过数、中位数、配对及产物校验结果。
- [logs/](logs/)：逐次 stdout、stderr 和 gprof 产物。

已核对全部参数组合、源文件与二进制、四份原始CSV校验和、内存估算和扣锁计算、各类BPK、汇总中位数及有效样本数。

在项目根复现（默认创建新目录）：

```sh
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```
