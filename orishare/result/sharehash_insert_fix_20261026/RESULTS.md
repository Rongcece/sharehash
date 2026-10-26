# ShareHash 插入与迁移修复验证

本次只修改 `include/ShareHash.h` 的右借位、扩容迁移调用和迁移辅助函数；新增回归测试。所有实现修改均由 `[修改1]`、`[修改2]`、`[修改3]` 注释标记。原五类哈希评测结果保留在 `../single_thread_20261025/`，本目录是修复后的 ShareHash 单独复测。

## 修改内容

1. **右借位 `[修改1]`**：右桶首槽占用时，先把该元素搬到右桶内部空槽，更新位图，再把腾空的首槽交给左桶。首槽本来为空时直接交接；同槽只获取一次锁。两桶占用计数不变，最终插入新键时才增加目标桶计数。零容量中间桶也能传递空位。
2. **迁移失败回滚 `[修改2]`**：每条迁移都检查返回值。任一条失败时恢复旧顶层结构和键值数组指针，释放候选表的键值、桶信息、位图、锁及顶层结构，然后返回失败。原表数据不因候选失败而释放。
3. **迁移路径一致 `[修改3]`**：`direct_insert_during_resize` 改为返回 bool，并复用 `insert(key, value, true)`。不再把溢出键放到不属于它的桶；保留迁移期间不重入全局锁、不递归扩容的语义。

`get()` 仍只扫描哈希算出的本桶。本次逐段比对确认初始化、哈希定义、查询、删除、`checkBucket` 和内存统计均未改动；只向右搜索、SEARCHRANGE=500（最多499个后继桶）、每次候选容量翻倍等原规则也保持不变。

## 验证

- 新增 `benchmark/sharehash_insert_regression.cpp`，11 项公共 API 回归全部通过，检查已插入键的实际返回值。覆盖原 8 键漏查、空/占用首槽、多桶传递、零容量桶、删除重插、迁移本桶超过4条、两种失败回滚、多次自然扩容和固定种子随机10万键。
- 使用修复前头文件编译同一测试，原 8 键用例和扩容迁移用例分别因漏查 key 0、key 7 失败；修复后通过。
- ASan/UBSan 下 11 项全部通过，`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1`。
- 单独启用泄漏检查时，11 项功能检查运行结束后，LeakSanitizer 因 ptrace 环境限制报 fatal error，未能完成泄漏检查；没有将其计为通过。
- 单线程复测：10万、100万、500万各3次，9/9 次的插入成功数和查询命中数均等于请求条数。普通评测只检查命中计数；完整键值校验由回归测试执行。
- CSV 文件校验和、源码校验和、参数及完整行数均已核对。详情见 `validation.json` 和 `run_manifest.json`。

## 单线程结果

运行时间：2026-10-26 13:41:19 至 2026-10-26 13:41:30（北京时间，UTC+8）。保留原 `-pg -g -O1` 编译选项，关闭 ASan/TSan；CPU 绑定与数据种子沿用旧评测。以下为三次中位数。BPK 单位为 bytes/key，heap/RSS 均为剔除输入生成占用后的估计值。

| 条数 | 检查通过 | 分配器 BPK | RSS BPK | 插入 Mops/s | 查询 Mops/s |
|---:|---:|---:|---:|---:|---:|
| 100,000 | 3/3 | 32.258 | 38.666 | 3.444 | 10.775 |
| 1,000,000 | 3/3 | 25.766 | 29.151 | 3.985 | 7.772 |
| 5,000,000 | 3/3 | 41.218 | 46.258 | 2.877 | 5.092 |

旧结果存在漏查，不能将其吞吐量直接作为正确索引的有效性能基线。当前改动主要解决布局正确性；没有承诺消除所有容量放大。

## 保留限制和需要后续决定的问题

- 右边界及 SEARCHRANGE 仍可能在全表未满时触发扩容；未增加向左借位、首尾环绕或全局装载率策略，所以高 BPK 的部分原因仍在。
- 一次翻倍候选也可能因搜索范围限制而迁移失败。测试用固定整数哈希构造了有效旧表：home0先获得2000槽，通过桶499借用/删除/再次借位获得第2001槽；重建候选时受初始布局及499个后继桶限制，第2001条不能迁入。本次返回false并保留旧表，不增加自动连续翻倍重试策略。
- 重复键语义、全局锁策略、初始化参数、内存统计以及其他四种哈希均未修改。

## 文件与复现

- [memory_summary.csv](memory_summary.csv)：插入后内存/BPK汇总。
- [summary.csv](summary.csv)：吞吐量与命中检查汇总。
- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：9次原始记录。
- [run_manifest.json](run_manifest.json)：配置、命令、退出码、源码及产物校验和。
- [validation.json](validation.json)：回归及sanitizer检查结果和限制。

```sh
c++ -std=c++14 -O2 -pthread benchmark/sharehash_insert_regression.cpp -o /tmp/sharehash_insert_regression
/tmp/sharehash_insert_regression
cmake --build build-single-thread --target benchmark -j4
python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark --hashmaps sharehash
```
