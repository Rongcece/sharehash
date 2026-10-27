# 六类哈希单线程基准：共享页池 ShareHash

共 54 次独立进程运行，54 次通过，0 次失败。此次不运行 SpaceLimitA 和 MapEmbed；共享池版本直接使用 `include/ShareHash_pagepool.h` 默认参数，未修改算法头文件。

## 配置

- 100,000 / 1,000,000 / 5,000,000 条，每类每规模 3 次；种子 20261025 / 20261026 / 20261027。
- 随机 uint64 键值，键等于值、未去重，载荷 16 bytes/条；每个配对种子输入相同。
- 单线程，CPU 0，OMP_NUM_THREADS=1，OMP_DYNAMIC=FALSE；逐进程顺序运行。
- 沿用 -std=gnu++14 -pg -g -O1 -fopenmp，ASan/TSan 关闭。
- 正确性检查为插入成功数与查询命中数均达到 N，未逐条校验返回值。
- 共享池默认 T=32，128 槽/页，初始 20,480 槽；最大负载 0.8、最小负载参数 0.75，稳定/迁移预算系数均为 32 bytes/已存记录。物理上限为 size_t 最大值，线性准入约束仍启用。
- 主内存指标为扣锁后的分配器估计，包含 KV 载荷；RSS 和吞吐量保留实际带锁测量。单表扣 capacity+1、普通子表扣 capacity+T、共享池扣 physical_capacity+T+1 个 SpinLock（本机 1 byte）。
- heap/RSS 索引估计为 (after_init-before_init)+(after_insert-before_insert)，排除输入生成占用；账本为自有请求字节，区别于 heap/RSS。

- started_utc：2026-10-27T18:36:25+08:00（北京时间）。
- finished_utc：2026-10-27T18:50:21+08:00（北京时间）。

## 三次中位数

仅使用 status=passed 样本；BPK=bytes/key，MiB=2^20 bytes，吞吐量单位 Mops/s。

| 条数 | 实现 | 通过 | 扣锁 heap MiB | 扣锁 heap BPK | 实测 RSS BPK | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|---:|---:|---:|
| 100,000 | ShareHash 单表 | 3/3 | 2.926 | 30.682 | 38.011 | 3.428 | 10.718 |
| 100,000 | ShareHash 普通子表 | 3/3 | 3.345 | 35.076 | 43.377 | 4.713 | 11.923 |
| 100,000 | ShareHash 共享池 | 3/3 | 3.131 | 32.832 | 36.127 | 4.419 | 1.691 |
| 100,000 | unordered_map | 3/3 | 4.377 | 45.899 | 47.923 | 7.098 | 27.252 |
| 100,000 | CuckooHash | 3/3 | 8.512 | 89.252 | 90.972 | 7.798 | 10.940 |
| 100,000 | SparseHash | 3/3 | 1.826 | 19.147 | 23.265 | 2.879 | 12.129 |
| 1,000,000 | ShareHash 单表 | 3/3 | 23.354 | 24.488 | 29.086 | 4.100 | 8.365 |
| 1,000,000 | ShareHash 普通子表 | 3/3 | 32.143 | 33.704 | 38.007 | 3.977 | 8.684 |
| 1,000,000 | ShareHash 共享池 | 3/3 | 29.542 | 30.978 | 32.428 | 3.231 | 0.169 |
| 1,000,000 | unordered_map | 3/3 | 41.566 | 43.585 | 43.786 | 5.269 | 16.903 |
| 1,000,000 | CuckooHash | 3/3 | 22.012 | 23.081 | 23.249 | 2.753 | 9.888 |
| 1,000,000 | SparseHash | 3/3 | 16.892 | 17.712 | 18.874 | 2.322 | 11.450 |
| 5,000,000 | ShareHash 单表 | 3/3 | 186.779 | 39.170 | 46.265 | 2.868 | 5.175 |
| 5,000,000 | ShareHash 普通子表 | 3/3 | 186.845 | 39.184 | 43.667 | 3.015 | 5.737 |
| 5,000,000 | ShareHash 共享池 | 3/3 | 146.976 | 30.823 | 32.130 | 1.909 | 0.020 |
| 5,000,000 | unordered_map | 3/3 | 198.120 | 41.549 | 41.589 | 3.217 | 9.563 |
| 5,000,000 | CuckooHash | 3/3 | 148.012 | 31.040 | 31.082 | 2.016 | 5.375 |
| 5,000,000 | SparseHash | 3/3 | 82.161 | 17.231 | 18.017 | 2.149 | 6.340 |

共享池保持固定逻辑桶数量，增长时追加物理页。源码中查询沿桶内槽链遍历；随着数据量增加链变长，可能是查询吞吐量下降的重要原因。本轮未做算法优化或额外剖析。

## 共享池预算诊断

| 请求 N | 已存记录中位数 | 拒绝数中位数 | 当前账本 MiB | 历史峰值账本 MiB |
|---:|---:|---:|---:|---:|
| 100,000 | 100,000 | 0 | 3.239 | 3.239 |
| 1,000,000 | 1,000,000 | 0 | 30.655 | 30.655 |
| 5,000,000 | 5,000,000 | 0 | 152.549 | 152.549 |

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
