# PagePool B 实现与验证

实现文件：[ShareHash_pagepoolB.h](../../include/ShareHash_pagepoolB.h)。从 V2-A 保留公开操作、重复键语义、全局预算先于子表锁的顺序、物理/线性准入、失败保护及不缩容语义。旧 V2-A、旧共享池头文件和默认六类 benchmark 均未修改。

## 为什么需要调整方案

只追加物理页而保留固定逻辑桶数，会重现旧共享池的 N/5000 长链；仅把数组换成页寻址无法保证快速查询。B 将记录存储与检索索引分离：

- 全局 PagePool 拥有固定 128 槽的 Page，含原位构造的 KV、位图和槽锁。子表页目录映射本地页 ID，记录页与记录地址在插入/增长期间保持稳定。
- 页目录和索引桶目录均使用固定四层、每层 64 项的 radix directory。每次访问固定四层，追加至多申请四个 512-byte 节点；不扫描目录链表，也不复制整张目录。
- 线性哈希索引按需逐桶分裂。索引项为 8-byte Handle：32-bit 本地槽号和缓存的 32-bit 路由哈希。槽号经 page_id=slot/128、offset=slot%128 寻址。
- 查询先在短索引桶中匹配缓存哈希，通常只对命中的句柄访问一个 KV 页，再比较完整 Key。32-bit 哈希相同也必须比较 Key，不会产生错误命中。
- 分裂只重新连接已有句柄，不搬动 KV、不重新调用旧键的 hash。利用旧溢出块原位分配左右桶的句柄容量，至多追加一个固定索引桶块及目录节点，没有完整新旧索引共存。
- 因记录与索引独立，原来的“连续 KV 区间逐槽借位”改为借用索引溢出块容量并逐桶分裂；没有保留导致长链的固定逻辑桶设计。插入不需要跨页搬移 KV。
- get 持目标子表锁，足以保护页目录、索引及记录；不在每个无关候选上执行槽锁。写入沿用 budget_lock → table_lock → slot_lock 顺序。reset/delete/destruction 不支持与其他操作并发。

这是均匀哈希下的预期常数时间检索，不是零延迟或恶意全碰撞下的最坏常数时间保证。碰撞溢出链仍可能变长，局部索引分裂也有写入开销。

## 预算与空间口径

本机 uint64 KV：对象 128 bytes，Page 2216 bytes，radix 节点 512 bytes，16 桶的索引块 1544 bytes，溢出索引块 80 bytes，Handle 8 bytes。默认 T=32，初始 20480 个物理槽，初始总请求字节 b=983720。实际分配均在预留预算后发生，包含索引、目录、控制块和锁；allocator 自身开销不计入内部账本。

默认 a_stable=a_migration=48 bytes/已存记录，b 自动取完整初始化请求字节。48 是含索引的可配置默认准入系数，不宣称一定小于旧版 32，也不保证任意输入都不会触发拒绝。自定义 SpaceBudget 可以配置两个系数、b 和有限物理上限；显式 b 不足时初始化拒绝。

min_load_factor=0.75 对新增物理容量使用启动余量：n >= alpha_min*(total_slots-initial_slots)。初始容量由 b 承担，避免小规模/子表偏斜导致过早拒绝；它不同于 V2-A 对整表翻倍后全容量检查 0.75 的规则。

单次页增长的增量至多 sizeof(Page)+4*sizeof(radix_node)；索引目录跨界增长至多 sizeof(BucketBlock)+4*sizeof(radix_node)。没有与当前子表容量成正比的复制峰值。每个阶段同时检查稳定、峰值系数和物理限额，因此允许 a_migration=a_stable。

理论式中的 q 必须扩展为包含句柄索引及目录的有效成本，不能只取 KV+bitmap+lock。随页数增长的管理元数据也不能全部算进常数 b。删除保留页/索引容量，可能令当前字节超过减少后的 a*n+b；后续写入必须重新准入。历史 peak 不会因删除或 reset 自动降低。reset 构建候选完整存储时仍有新旧存储共存，并单独检查预算，不属于普通页增长的 O(P) 峰值结论。

## 功能与内存安全验证

[pagepoolB_regression.cpp](../../benchmark/pagepoolB_regression.cpp) 共 9 组检查全部通过：

- 随机完整值校验，增长前后记录地址不变，删除/空位复用/重置/释放后再初始化。
- 极端同哈希不同键、重复键的值和删除语义。
- 通过计数 hash 调用验证插入每条键只 hash 一次，分裂不重新 hash 旧记录。
- 零斜率预算、有限硬上限、显式不足 b、非法参数和手动增长准入。
- 初始化、页目录跨界、索引桶增长的分配失败注入，保留旧记录并可重试。
- reset 分配失败时旧记录、地址、数量和账本保持有效。
- 目录跨越 64/4096/262144 项边界的完整寻址验证。
- 并发插入/查询和删除检查。
- 用 operator new/delete 统计全部索引请求字节，与 current_live_bytes-sizeof(index) 比对；每组测试结束索引分配净额为零。

ASan+UBSan 同样全部通过，见 [asan_ubsan.log](asan_ubsan.log)。此环境 LeakSanitizer 因 ptrace 限制无法运行，因此 ASAN_OPTIONS=detect_leaks=0；请求字节/泄漏净额另由上述分配探针验证。未将其表述为 LeakSanitizer 通过。

## 针对性性能对照

[pagepoolB_benchmark.cpp](../../benchmark/pagepoolB_benchmark.cpp) 在相同二进制中独立类型隔离 B 和普通子表，逐进程顺序运行。CPU 0，-std=gnu++14 -pg -g -O1 -fopenmp，种子 20261025/26/27，每规模每类三次，共 18/18 通过全量插入、命中及返回值检查。三次中位数如下；Mops/s=百万次/秒，heap BPK 扣锁且含 KV。

两类均使用各自默认参数，B 默认 T=32，普通子表 T=16。此程序直接调用具体类型，并在查询计时中验证值；与旧通用基准的虚函数调用/仅命中计数不同。只能在本表内直接比较，不能把旧版约 1.99 万次/秒与本表相除作严格加速比。没有重新运行原六类比较。

| N | 实现 | 插入 Mops/s | 查询 Mops/s | 扣锁 heap BPK |
|---:|---|---:|---:|---:|
| 100,000 | subtable | 5.221 | 16.365 | 35.059 |
| 100,000 | pagepoolB | 6.989 | 17.265 | 41.344 |
| 1,000,000 | subtable | 4.444 | 11.969 | 33.703 |
| 1,000,000 | pagepoolB | 5.259 | 14.530 | 38.967 |
| 5,000,000 | subtable | 3.503 | 7.941 | 39.184 |
| 5,000,000 | pagepoolB | 3.944 | 9.851 | 39.605 |

B 消除了固定桶数和线性页目录导致的查询退化，但付出了新增句柄索引的内存代价。500 万条 B 的扣锁 heap 约 39.61 BPK，高于旧共享池约 30.82 BPK，与本轮普通子表约 39.18 BPK 接近；这不是更省内存的结论。

## 使用与复现

作为替代头文件，单独包含 ShareHash_pagepoolB.h，类型仍为 ShareHash::sharehash_subtable<Key,Value>。如需与其他变体共存，按现有适配层惯例在 include 前后宏重命名 sharehash_subtable。公开 get/insert/remove/resize、SpaceBudget、计数和分表快照接口保留；无连续数组视图，SubtableStats 的旧数组指针字段返回 nullptr，新增 page_count/index_split_count/relinked_handles。migrated_items 始终为零。

记录须 trivially_copyable 且支持不抛异常的复制构造，C++14 下不支持超过 max_align_t 的记录对齐。槽句柄和四层目录的边界会在增长前检查，不静默回绕。

仓库根目录执行：

```sh
c++ -std=c++14 -O1 -g -pthread orishare/benchmark/pagepoolB_regression.cpp -o /tmp/pagepoolB_regression
/tmp/pagepoolB_regression
c++ -std=c++14 -O1 -g -pthread -fsanitize=address,undefined -fno-omit-frame-pointer orishare/benchmark/pagepoolB_regression.cpp -o /tmp/pagepoolB_regression_asan
ASAN_OPTIONS=detect_leaks=0 /tmp/pagepoolB_regression_asan
c++ -std=gnu++14 -pg -g -O1 -fopenmp orishare/benchmark/pagepoolB_benchmark.cpp -o /tmp/pagepoolB_benchmark
/tmp/pagepoolB_benchmark pagepoolB 5000000 20261025
```

[run_focused.py](run_focused.py) 可复现全部对照，但会拒绝覆盖已有 focused_results.csv；复跑请复制脚本到新的 result 子目录。原始数据见 [focused_results.csv](focused_results.csv)，中位数见 [focused_summary.csv](focused_summary.csv)，源码与二进制校验和及命令见 [focused_manifest.json](focused_manifest.json)。first_run/ 保留最终 C++14 常量定义补齐前的一轮开发测量，不作为本报告主结果。
