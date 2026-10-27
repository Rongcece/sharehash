# 七类哈希统一单线程基准：加入 SpaceLimitA

已完成 7 类哈希 × 3 个规模 × 3 次重复，共 **63 次独立进程运行，45 次通过，18 次未通过全量检查**。单表 ShareHash、修复后子表 ShareHash、unordered_map、CuckooHash、SparseHash 各 9/9 通过。SpaceLimitA 9 次均正常退出并通过预算诊断，但只接受部分数据，未通过全量插入/命中检查；MapEmbed 9 次因原有栈破坏崩溃。

本轮将 SpaceLimitA 注册为独立 `sharehash_spacelimitA`。其头文件与普通子表版本原本使用同一模板名，适配层仅在包含时隔离类型名，避免 ODR 冲突；三个算法头文件均未修改，原有六类算法也保持原版本。

## 统一配置

- 规模：100,000 / 1,000,000 / 5,000,000；每类每规模重复 3 次，使用种子 20261025 / 20261026 / 20261027。
- 相同规模与重复的七类索引使用同一随机 uint64 键值对，键等于值，未去重，载荷 16 bytes/条；操作顺序与计时区间沿用原基准。
- 单线程 `-p 1`、CPU 0，`OMP_NUM_THREADS=1`、`OMP_DYNAMIC=FALSE`；逐进程顺序运行。
- Intel(R) Xeon(R) Gold 5320 CPU @ 2.20GHz；glibc 2.31。
- 保留 `-std=gnu++14 -pg -g -O1 -fopenmp`，ASan/TSan 关闭。
- 时间：2026-10-27 14:25:05 至 2026-10-27 14:26:13（北京时间，UTC+8）。
- 基准正确性为插入成功数及查询命中数达到请求 N；本轮没有逐条验证返回值内容。

## 三种 ShareHash 配置与内存口径

| 版本 | T | 初始总槽 | 最大负载 | 扩容后最小全表负载 | 稳定/迁移系数 | b |
|---|---:|---:|---:|---:|---|---|
| 单表 | 1 | 20,000 | 原策略 | 无额外约束 | 无 | 无 |
| 修复后子表 | 16 | 20,000 | 0.8 | 无额外约束 | 无 | 无 |
| SpaceLimitA | 32 | 20,000 | 0.8 | 0.75 | 32 / 64 bytes/已存记录 | 404,944 bytes |

**SpaceLimitA 使用当前默认参数，没有配置有限的外部物理上限。** `physical_limit_bytes=18446744073709551615`（64-bit size_t 最大值）；其 `a*n+b` 线性限额及最小负载准入仍然有效。不能将本轮表述成一个有限物理内存上限实验。V2-A 无后备区。

主比较继续使用扣除 ShareHash 锁对象字节后的分配器估计。单表扣 `(capacity+1)*sizeof(SpinLock)`；普通子表扣 `(capacity+T)*sizeof(SpinLock)`；SpaceLimitA 扣 `(capacity+T+1)*sizeof(SpinLock)`，额外的 1 是全局预算锁。本机 SpinLock=1 byte。原始含锁列保留，padding、锁指针与分配器元数据不扣。其他四类不做锁扣除，0 不代表没有内部锁。

heap/RSS 原始索引估计采用 `(after_init-before_init)+(after_insert-before_insert)` 排除实测输入生成占用。RSS 与吞吐量保持实际带锁测量。V2-A 的内部账本及历史峰值包含锁，只约束索引自有请求字节，不是进程 RSS，也不包含分配器内部开销。

## 完整数据集的有效内存比较

只使用 `status=passed` 的样本，数值为三次中位数。BPK=bytes/key，包含 KV 载荷；MiB=2^20 bytes。SpaceLimitA 与 MapEmbed 没有完整有效样本，故列空；不是 0。

| 条数 | 索引 | 通过 | 扣锁分配器 MiB | 扣锁分配器 BPK | 实测 RSS BPK（含锁） |
|---:|---|---:|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 2.924 | 30.658 | 38.871 |
| 100,000 | ShareHash（修复后子表） | 3/3 | 3.343 | 35.054 | 43.377 |
| 100,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — | — |
| 100,000 | unordered_map | 3/3 | 4.375 | 45.876 | 47.800 |
| 100,000 | MapEmbed | 0/3 | — | — | — |
| 100,000 | CuckooHash | 3/3 | 8.509 | 89.228 | 90.808 |
| 100,000 | SparseHash | 3/3 | 1.826 | 19.146 | 23.142 |
| 1,000,000 | ShareHash（单表） | 3/3 | 23.352 | 24.486 | 29.086 |
| 1,000,000 | ShareHash（修复后子表） | 3/3 | 32.141 | 33.702 | 37.925 |
| 1,000,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — | — |
| 1,000,000 | unordered_map | 3/3 | 41.564 | 43.583 | 43.770 |
| 1,000,000 | MapEmbed | 0/3 | — | — | — |
| 1,000,000 | CuckooHash | 3/3 | 22.009 | 23.079 | 23.278 |
| 1,000,000 | SparseHash | 3/3 | 16.892 | 17.712 | 18.883 |
| 5,000,000 | ShareHash（单表） | 3/3 | 186.777 | 39.170 | 46.253 |
| 5,000,000 | ShareHash（修复后子表） | 3/3 | 186.843 | 39.184 | 43.654 |
| 5,000,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — | — |
| 5,000,000 | unordered_map | 3/3 | 198.118 | 41.548 | 41.587 |
| 5,000,000 | MapEmbed | 0/3 | — | — | — |
| 5,000,000 | CuckooHash | 3/3 | 148.009 | 31.040 | 31.080 |
| 5,000,000 | SparseHash | 3/3 | 82.161 | 17.231 | 18.028 |

## 完整数据集的有效吞吐量

单位 Mops/s，三次中位数。SpaceLimitA 的部分接受运行不参与本表。

| 条数 | 索引 | 通过 | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|
| 100,000 | ShareHash（单表） | 3/3 | 3.521 | 10.866 |
| 100,000 | ShareHash（修复后子表） | 3/3 | 4.783 | 11.806 |
| 100,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — |
| 100,000 | unordered_map | 3/3 | 7.105 | 27.344 |
| 100,000 | MapEmbed | 0/3 | — | — |
| 100,000 | CuckooHash | 3/3 | 6.928 | 11.094 |
| 100,000 | SparseHash | 3/3 | 2.842 | 11.976 |
| 1,000,000 | ShareHash（单表） | 3/3 | 4.088 | 8.370 |
| 1,000,000 | ShareHash（修复后子表） | 3/3 | 3.997 | 8.706 |
| 1,000,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — |
| 1,000,000 | unordered_map | 3/3 | 5.286 | 17.036 |
| 1,000,000 | MapEmbed | 0/3 | — | — |
| 1,000,000 | CuckooHash | 3/3 | 2.709 | 10.003 |
| 1,000,000 | SparseHash | 3/3 | 2.323 | 10.994 |
| 5,000,000 | ShareHash（单表） | 3/3 | 2.855 | 5.174 |
| 5,000,000 | ShareHash（修复后子表） | 3/3 | 3.089 | 5.698 |
| 5,000,000 | ShareHash SpaceLimitA（V2-A） | 0/3 | — | — |
| 5,000,000 | unordered_map | 3/3 | 3.238 | 9.588 |
| 5,000,000 | MapEmbed | 0/3 | — | — |
| 5,000,000 | CuckooHash | 3/3 | 2.019 | 5.436 |
| 5,000,000 | SparseHash | 3/3 | 2.154 | 6.075 |

## SpaceLimitA 预算与部分接受诊断

九次预算 CSV 校验均通过：当前字节不高于稳定线性限额、记账峰值不高于迁移线性限额，成功/拒绝数、账本记录数及三个比率一致。预算诊断通过与全量插入检查通过是两个不同结果。

下表为每个规模三次诊断的中位数。**BPK 分母是实际已存记录数**；reported 是内部请求字节账本，heap 是分配器估计。这些数值仅用于分析部分接受的结构，不应与上面的完整数据集结果直接排名。历史峰值保持含锁口径。

| 请求 N | 已存记录 | 接受率 | 拒绝记录 | 当前账本 MiB | 峰值账本 MiB | 扣锁账本 BPK/已存记录 | 扣锁 heap BPK/已存记录 |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 100,000 | 27,338 | 27.338% | 72,662 | 0.698 | 0.710 | 25.435 | 25.757 |
| 1,000,000 | 153,331 | 15.333% | 846,669 | 3.840 | 3.936 | 24.954 | 25.205 |
| 5,000,000 | 716,434 | 14.329% | 4,283,566 | 18.034 | 18.418 | 25.084 | 25.263 |

主 CSV 仍统一按请求 N 计算 BPK。SpaceLimitA 只存下部分键，按 N 得到的较小 BPK 不能解释为空间优势。原始插入吞吐量也按 N/总插入耗时计算，属于尝试速率；预算 CSV 另记成功插入数/总插入耗时，仍只作为诊断。

各次 SpaceLimitA 的具体接受数如下；查询命中数与成功插入数相等，没有出现额外的命中计数损失。该计数关系不替代完整返回值校验。

| N | 种子 | 插入成功 | 拒绝 | 查询命中 | 预算检查 |
|---:|---:|---:|---:|---:|---|
| 100,000 | 20261025 | 27,338 | 72,662 | 27,338 | 通过 |
| 100,000 | 20261026 | 27,179 | 72,821 | 27,179 | 通过 |
| 100,000 | 20261027 | 27,420 | 72,580 | 27,420 | 通过 |
| 1,000,000 | 20261025 | 152,989 | 847,011 | 152,989 | 通过 |
| 1,000,000 | 20261026 | 153,331 | 846,669 | 153,331 | 通过 |
| 1,000,000 | 20261027 | 153,494 | 846,506 | 153,494 | 通过 |
| 5,000,000 | 20261025 | 715,489 | 4,284,511 | 715,489 | 通过 |
| 5,000,000 | 20261026 | 716,434 | 4,283,566 | 716,434 | 通过 |
| 5,000,000 | 20261027 | 716,750 | 4,283,250 | 716,750 | 通过 |

最终快照显示 SpaceLimitA 没有达到物理最大值，当前/历史峰值也低于对应最终线性限额。不能将这些拒绝直接归因于物理内存耗尽；最小负载、局部借位和迁移准入等规则仍会限制插入，本轮没有对每次拒绝原因额外插桩。

## MapEmbed 与产物状态

- MapEmbed 9 次均 SIGABRT，退出码 -6，stderr 为 `stack smashing detected`。10 万与 100 万条共 6 次有插入后快照；500 万条 3 次没有。
- `benchmark_results.csv` 共 54 条完成查询记录，其中 9 条是部分插入的 SpaceLimitA；`memory_results.csv` 共 60 条快照，还包含 6 条失败 MapEmbed。
- `summary.csv`、`memory_summary.csv` 各 21 组，按原有策略保留失败数字作诊断，不能只看数值而忽略通过数。主表使用 `valid_only_*`。
- 全部 63 次计划任务完成，无超时或中断。runner 退出码 1、总状态 failed，对应 9 次部分插入和 9 次 MapEmbed 崩溃。

## 文件与复现

- [RESULTS.md](RESULTS.md)：本报告。
- [valid_only_memory_summary.csv](valid_only_memory_summary.csv)、[valid_only_summary.csv](valid_only_summary.csv)：只含完整通过样本的七类汇总。
- [space_budget_results.csv](space_budget_results.csv)：SpaceLimitA 九次原始预算诊断与实际配置。
- [space_budget_summary.csv](space_budget_summary.csv)：按规模汇总的接受率、字节及已存记录 BPK。
- [sharehash_matched_comparison.csv](sharehash_matched_comparison.csv)：单表与普通子表的三个配对种子比较。
- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：保留失败样本的原始统一 CSV。
- [summary.csv](summary.csv)、[memory_summary.csv](memory_summary.csv)：标准诊断汇总。
- [run_manifest.json](run_manifest.json)：命令、退出码、参数、预算行、源码/二进制/五份运行CSV校验和。
- [validation.json](validation.json)：原始/扣锁/预算公式、中位数、有效样本及产物校验结果。
- [logs/](logs/)：逐进程 stdout/stderr 与 gprof 产物。

在项目根执行以下命令，默认创建新目录并运行七类索引：

```sh
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```

仅运行独立 V2-A（仍使用默认预算配置）：

```sh
./build-single-thread/benchmark -t sharehash_spacelimitA -d 1000000 -p 1 \
  --seed 20261025 --result-dir /tmp/sharehash_spacelimitA_result
```
