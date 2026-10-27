# 六类哈希单线程基准：共享页池 B ShareHash

共 54 次独立进程运行，54 次通过，0 次失败。此次不运行 SpaceLimitA 和 MapEmbed；共享池版本直接使用 `include/ShareHash_pagepoolB.h` 默认参数，未修改算法头文件。

## 配置

- 100,000 / 1,000,000 / 5,000,000 条，每类每规模 3 次；种子 20261025 / 20261026 / 20261027。
- 随机 uint64 键值，键等于值、未去重，载荷 16 bytes/条；每个配对种子输入相同。
- 单线程，CPU 0，OMP_NUM_THREADS=1，OMP_DYNAMIC=FALSE；逐进程顺序运行。
- 沿用 -std=gnu++14 -pg -g -O1 -fopenmp，ASan/TSan 关闭。
- 正确性检查为插入成功数与查询命中数均达到 N，未逐条校验返回值。
- 共享池默认 T=32，128 槽/页，初始 20,480 槽；最大负载 0.8、最小负载参数 0.75，稳定/迁移预算系数均为 48 bytes/已存记录。物理上限为 size_t 最大值，线性准入约束仍启用。
- 主内存指标为扣锁后的分配器估计，包含 KV 载荷；RSS 和吞吐量保留实际带锁测量。单表扣 capacity+1、普通子表扣 capacity+T、共享池扣 physical_capacity+T+1 个 SpinLock（本机 1 byte）。
- heap/RSS 索引估计为 (after_init-before_init)+(after_insert-before_insert)，排除输入生成占用；账本为自有请求字节，区别于 heap/RSS。

- started_utc：2026-10-27T19:24:48+08:00（北京时间）。
- finished_utc：2026-10-27T19:25:50+08:00（北京时间）。

原共享池结果归档于 [archive_pagepool_original/RESULTS.md](archive_pagepool_original/RESULTS.md)，本报告和根目录 CSV 为 B 版六类重跑结果。

## 三次中位数

仅使用 status=passed 样本；BPK=bytes/key，MiB=2^20 bytes，吞吐量单位 Mops/s。

| 条数 | 实现 | 通过 | 扣锁 heap MiB | 扣锁 heap BPK | 实测 RSS BPK | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|---:|---:|---:|
| 100,000 | ShareHash 单表 | 3/3 | 2.926 | 30.682 | 38.011 | 3.389 | 10.862 |
| 100,000 | ShareHash 普通子表 | 3/3 | 3.345 | 35.076 | 43.377 | 4.625 | 13.574 |
| 100,000 | ShareHash 共享池 B | 3/3 | 3.946 | 41.374 | 44.196 | 5.922 | 13.466 |
| 100,000 | unordered_map | 3/3 | 4.377 | 45.899 | 48.783 | 7.150 | 26.550 |
| 100,000 | CuckooHash | 3/3 | 8.512 | 89.252 | 91.300 | 7.273 | 10.856 |
| 100,000 | SparseHash | 3/3 | 1.826 | 19.147 | 22.610 | 2.842 | 12.045 |
| 1,000,000 | ShareHash 单表 | 3/3 | 23.354 | 24.488 | 29.082 | 4.085 | 8.330 |
| 1,000,000 | ShareHash 普通子表 | 3/3 | 32.143 | 33.704 | 37.925 | 3.936 | 9.712 |
| 1,000,000 | ShareHash 共享池 B | 3/3 | 37.165 | 38.970 | 40.423 | 4.645 | 10.630 |
| 1,000,000 | unordered_map | 3/3 | 41.566 | 43.585 | 43.872 | 5.289 | 16.732 |
| 1,000,000 | CuckooHash | 3/3 | 22.012 | 23.081 | 23.253 | 2.735 | 9.537 |
| 1,000,000 | SparseHash | 3/3 | 16.892 | 17.712 | 18.809 | 2.320 | 11.466 |
| 5,000,000 | ShareHash 单表 | 3/3 | 186.779 | 39.170 | 46.266 | 2.865 | 5.159 |
| 5,000,000 | ShareHash 普通子表 | 3/3 | 186.845 | 39.184 | 43.667 | 3.065 | 6.507 |
| 5,000,000 | ShareHash 共享池 B | 3/3 | 188.855 | 39.606 | 40.891 | 3.555 | 7.264 |
| 5,000,000 | unordered_map | 3/3 | 198.120 | 41.549 | 41.589 | 3.235 | 9.507 |
| 5,000,000 | CuckooHash | 3/3 | 148.012 | 31.040 | 31.081 | 2.023 | 5.386 |
| 5,000,000 | SparseHash | 3/3 | 82.161 | 17.231 | 18.017 | 2.144 | 6.334 |

共享池 B 使用固定四层页目录和按需分裂的线性哈希索引；增长仅调整缓存哈希/槽位句柄，不搬移 KV 记录，也不重新哈希旧记录。默认 min_load_factor 约束扣除初始容量后的新增物理容量，初始容量由 b 承担；索引、目录、控制块和锁均纳入预算。CLI/CSV 中保留 sharehash_pagepool 标识，本轮实际源文件为 ShareHash_pagepoolB.h。

## 共享池预算诊断

| 请求 N | 已存记录中位数 | 拒绝数中位数 | 当前账本 MiB | 历史峰值账本 MiB |
|---:|---:|---:|---:|---:|
| 100,000 | 100,000 | 0 | 3.985 | 3.985 |
| 1,000,000 | 1,000,000 | 0 | 37.718 | 37.719 |
| 5,000,000 | 5,000,000 | 0 | 191.033 | 191.033 |

预算 CSV 已校验成功/拒绝数、账本/快照一致性、物理与线性上限及比率。历史峰值保持含锁口径。

## 产物与复现

- [summary.csv](summary.csv)、[memory_summary.csv](memory_summary.csv)：中位数及通过数。标准汇总会保留失败数值，需检查通过数。
- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：逐次原始测量。
- [space_budget_results.csv](space_budget_results.csv)：共享池实际配置与预算诊断。
- [run_manifest.json](run_manifest.json)：运行参数、源码与二进制校验和、逐次状态、命令及校验。
- [validation.json](validation.json)、[logs/](logs/)：校验结果和逐次日志。

在 orishare 目录执行：

```sh
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```
