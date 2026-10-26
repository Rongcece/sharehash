# 修复后 ShareHash 与其他四类哈希单线程评测

已完成 5 类哈希 × 3 个规模 × 3 次重复，共 **45 次独立进程运行**。修复后的 ShareHash、unordered_map、CuckooHash、SparseHash 各 **9/9 次通过**，插入返回成功数和查询命中数均等于请求条数，共 36 次通过。MapEmbed 9 次均因原有栈破坏退出，保留失败日志及已取得的内存快照。

本次使用已修复的 ShareHash 和原有四类对照实现。登记的 21 份源码及评测二进制 SHA256 与上一轮 ShareHash 修复后单独复测完全一致；沿用原初始化、哈希参数、操作顺序和测量逻辑。

## 运行配置

- 数据规模：100,000 / 1,000,000 / 5,000,000，每类每规模重复 3 次；表中数值均为中位数。
- 单线程 `-p 1`，绑定 CPU 0；`OMP_NUM_THREADS=1`、`OMP_DYNAMIC=FALSE`。所有任务依次执行。
- 三次重复的种子为 20261025 / 20261026 / 20261027；相同规模与重复的五类哈希使用相同数据。
- 随机 uint64 键值对，键等于值，每条载荷 16 bytes；未去重，BPK 分母为请求插入条数。
- CPU：Intel(R) Xeon(R) Gold 5320 CPU @ 2.20GHz；glibc 2.31。
- 保留原编译选项 `-pg -g -O1`，OpenMP 开启，ASan/TSan 关闭。吞吐量对应此编译配置。
- 运行时间：2026-10-26 15:25:22 至 2026-10-26 15:26:11（北京时间，UTC+8）。
- 本轮普通评测校验插入成功数与查询命中数；实际返回键值的校验见上一轮修复回归测试。

## 插入后内存与 BPK

以下取 `memory_summary.csv` 的 `after_insert` 快照。**BPK = bytes/key**，包括键值载荷。MiB = 2^20 bytes。分配器占用统计 live heap/mmap 分配，包含尚未触碰的预留数组；RSS 统计驻留页。两种索引内存估计均使用 `(after_init - before_init) + (after_insert - before_insert)`，排除实测输入生成占用，仍可能包含分配器和运行时开销。进程总 RSS 与峰值 RSS 另外保留在原始 CSV 中。

各实现原有 `memory_consumption()` 口径不同，例如 unordered_map 仅返回容器本体 56 bytes。因此同时保留原始、分配器及 RSS 三类 BPK，跨实现比较主要参考后两项。

**MapEmbed 的数值来自失败运行，只供诊断，不能作为正确索引的有效性能或内存结果。** 其 500 万条运行没有插入后快照；空值表示缺失。

| 条数 | 哈希 | 内存样本 | 分配器 MiB | 分配器 BPK | RSS MiB | RSS BPK | 原始 BPK | 完整检查通过 |
|---:|---|---:|---:|---:|---:|---:|---:|---:|
| 100,000 | ShareHash（修复后） | 3 | 3.076 | 32.257 | 3.707 | 38.871 | 32.2006 | 3/3 |
| 100,000 | unordered_map | 3 | 4.375 | 45.875 | 4.559 | 47.800 | 0.00056 | 3/3 |
| 100,000 | MapEmbed | 3 | 668.541 | 7010.157 | 210.977 | 2212.250 | 7010.01 | 0/3 |
| 100,000 | CuckooHash | 3 | 8.509 | 89.228 | 8.699 | 91.218 | 41.943 | 3/3 |
| 100,000 | SparseHash | 3 | 1.818 | 19.059 | 2.160 | 22.651 | 20.9715 | 3/3 |
| 1,000,000 | ShareHash（修复后） | 3 | 24.572 | 25.766 | 27.738 | 29.086 | 25.7601 | 3/3 |
| 1,000,000 | unordered_map | 3 | 41.563 | 43.582 | 41.746 | 43.774 | 5.6e-05 | 3/3 |
| 1,000,000 | MapEmbed | 3 | 668.541 | 701.016 | 210.977 | 221.225 | 701.001 | 0/3 |
| 1,000,000 | CuckooHash | 3 | 22.009 | 23.079 | 22.199 | 23.278 | 16.7772 | 3/3 |
| 1,000,000 | SparseHash | 3 | 16.890 | 17.711 | 18.066 | 18.944 | 33.5544 | 3/3 |
| 5,000,000 | ShareHash（修复后） | 3 | 196.543 | 41.218 | 220.609 | 46.265 | 41.216 | 3/3 |
| 5,000,000 | unordered_map | 3 | 198.118 | 41.548 | 198.301 | 41.587 | 1.12e-05 | 3/3 |
| 5,000,000 | MapEmbed | 0 | — | — | — | — | — | 0/3 |
| 5,000,000 | CuckooHash | 3 | 148.009 | 31.040 | 148.199 | 31.080 | 26.8435 | 3/3 |
| 5,000,000 | SparseHash | 3 | 82.161 | 17.230 | 85.809 | 17.995 | 26.8435 | 3/3 |

ShareHash 的分配器 BPK 中位数为 **32.257 / 25.766 / 41.218 bytes/key**，对应 10 万 / 100 万 / 500 万条。本轮这三个规模均通过命中计数检查；500 万条时其 RSS BPK 为 46.265，高于 unordered_map 的 41.587。SparseHash 在本轮三个规模的有效样本中分配器 BPK 最低。

## 插入与查询吞吐量

单位为 Mops/s（百万次操作/秒），取 `summary.csv` 中通过检查的记录，各为三次中位数。插入和查询的计时区间沿用原逻辑，包含返回值计数开销。MapEmbed 无通过检查的记录。

| 条数 | 哈希 | 通过次数 | 插入 Mops/s | 查询 Mops/s |
|---:|---|---:|---:|---:|
| 100,000 | ShareHash（修复后） | 3/3 | 3.393 | 10.595 |
| 100,000 | unordered_map | 3/3 | 6.675 | 26.987 |
| 100,000 | MapEmbed | 0/3 | — | — |
| 100,000 | CuckooHash | 3/3 | 6.519 | 10.837 |
| 100,000 | SparseHash | 3/3 | 2.809 | 12.035 |
| 1,000,000 | ShareHash（修复后） | 3/3 | 4.089 | 8.297 |
| 1,000,000 | unordered_map | 3/3 | 5.278 | 16.885 |
| 1,000,000 | MapEmbed | 0/3 | — | — |
| 1,000,000 | CuckooHash | 3/3 | 2.730 | 9.845 |
| 1,000,000 | SparseHash | 3/3 | 2.295 | 11.527 |
| 5,000,000 | ShareHash（修复后） | 3/3 | 2.811 | 5.179 |
| 5,000,000 | unordered_map | 3/3 | 3.224 | 9.579 |
| 5,000,000 | MapEmbed | 0/3 | — | — |
| 5,000,000 | CuckooHash | 3/3 | 2.016 | 5.397 |
| 5,000,000 | SparseHash | 3/3 | 2.144 | 6.021 |

旧版 ShareHash 的 9 次运行存在漏查，不能将其吞吐量当作正确索引的有效性能基线。旧结果保存在 [single_thread_20261025](../single_thread_20261025/RESULTS.md)，修复细节及回归测试见 [sharehash_insert_fix_20261026](../sharehash_insert_fix_20261026/RESULTS.md)。

## MapEmbed 失败情况

- 9/9 次收到 SIGABRT，Python 退出码均为 `-6`，stderr 均包含 `stack smashing detected`。
- 10 万与 100 万条共 6 次完成插入并保存内存快照，随后查询阶段退出。500 万条共 3 次在插入阶段退出，没有插入后快照。
- 先前源码检查已定位缓冲区大小与输出长度不匹配：[`murmur.h`](../../benchmark/competitors/MapEmbed/src/murmur.h) 的 128-bit 哈希输出写入 16 bytes，而 [`MapEmbed.h`](../../benchmark/competitors/MapEmbed/src/MapEmbed.h) 的部分调用仅提供 8-byte 或 4-byte 缓冲。相关实现沿用原样。
- 该实现还预留未使用的 480,000,160-byte KV 数组。分配器会计入这些空间，RSS 仅计入驻留部分，因而其失败样本中的约 701 MB 分配量与约 221 MB RSS 可以同时出现。
- runner 的退出码为 1，manifest 总状态为 `failed`，准确反映上述 9 次失败；全部 45 次计划任务均已执行，无超时或中断。

## 结果文件与核对

- [benchmark_results.csv](benchmark_results.csv)：36 条完成查询且通过检查的原始记录。
- [memory_results.csv](memory_results.csv)：42 条插入后快照，其中 6 条来自失败的 MapEmbed 运行。
- [summary.csv](summary.csv)：15 组吞吐量汇总及通过数。
- [memory_summary.csv](memory_summary.csv)：15 组插入后内存/BPK汇总及样本、通过数。
- [run_manifest.json](run_manifest.json)：全部 45 次运行的命令、参数、退出码、日志和源码/二进制/CSV校验和。
- [validation.json](validation.json)：本轮产物行数、校验和、参数、内存/BPK公式及中位数核对结果。
- [logs/](logs/)：逐次 stdout、stderr 及 gprof 产物。

四份 CSV 的 SHA256、45 次参数组合、三类 BPK 的 bytes/N 计算、heap/RSS 估算公式，以及全部汇总中位数和样本数均已核对。

复现命令（在项目根执行；默认创建新的结果目录）：

```sh
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark
```
