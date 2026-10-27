from pathlib import Path
import json,csv,statistics,hashlib
from datetime import datetime, timezone, timedelta
p=Path(__file__).resolve().parent
m=json.loads((p/'run_manifest.json').read_text())
assert m['status']!='running'
names={'sharehash':'ShareHash 单表','sharehash_subtable':'ShareHash 普通子表','sharehash_pagepool':'ShareHash 共享池','unorderedmap':'unordered_map','cuckoohash':'CuckooHash','sparsehash':'SparseHash'}
checks=[]
for file, expected in m['source_sha256'].items():
    assert hashlib.sha256((Path(m['project_root'])/file).read_bytes()).hexdigest()==expected
assert hashlib.sha256(Path(m['binary']).read_bytes()).hexdigest()==m['binary_sha256']
for file,key in [('benchmark_results.csv','benchmark_results_sha256'),('memory_results.csv','memory_results_sha256'),('space_budget_results.csv','space_budget_results_sha256'),('summary.csv','summary_sha256'),('memory_summary.csv','memory_summary_sha256')]:
    assert hashlib.sha256((p/file).read_bytes()).hexdigest()==m[key]
    checks.append(file)
for job in m['jobs']:
    if job['status']=='passed':
        assert int(job['csv_row']['insert_successes'])==job['dataset_size']==int(job['csv_row']['lookup_hits'])
assert not m['budget_diagnostic_errors']
for job in m['jobs']:
    if job['hashmap_type']=='sharehash_pagepool': assert not job['budget_validation_errors']
text=['# 六类哈希单线程基准：共享页池 ShareHash','',f"共 {m['expected_job_count']} 次独立进程运行，{m['passed_job_count']} 次通过，{m['failed_or_incomplete_job_count']} 次失败。此次不运行 SpaceLimitA 和 MapEmbed；共享池版本直接使用 `include/ShareHash_pagepool.h` 默认参数，未修改算法头文件。",'', '## 配置','', '- 100,000 / 1,000,000 / 5,000,000 条，每类每规模 3 次；种子 20261025 / 20261026 / 20261027。','- 随机 uint64 键值，键等于值、未去重，载荷 16 bytes/条；每个配对种子输入相同。',f"- 单线程，CPU {m['pinned_cpu']}，OMP_NUM_THREADS=1，OMP_DYNAMIC=FALSE；逐进程顺序运行。",'- 沿用 -std=gnu++14 -pg -g -O1 -fopenmp，ASan/TSan 关闭。','- 正确性检查为插入成功数与查询命中数均达到 N，未逐条校验返回值。','- 共享池默认 T=32，128 槽/页，初始 20,480 槽；最大负载 0.8、最小负载参数 0.75，稳定/迁移预算系数均为 32 bytes/已存记录。物理上限为 size_t 最大值，线性准入约束仍启用。','- 主内存指标为扣锁后的分配器估计，包含 KV 载荷；RSS 和吞吐量保留实际带锁测量。单表扣 capacity+1、普通子表扣 capacity+T、共享池扣 physical_capacity+T+1 个 SpinLock（本机 1 byte）。','- heap/RSS 索引估计为 (after_init-before_init)+(after_insert-before_insert)，排除输入生成占用；账本为自有请求字节，区别于 heap/RSS。','']
for key in ['started_utc','finished_utc']:
    text.append(f"- {key}：{datetime.fromisoformat(m[key]).astimezone(timezone(timedelta(hours=8))).isoformat()}（北京时间）。")
text+=['','## 三次中位数','', '仅使用 status=passed 样本；BPK=bytes/key，MiB=2^20 bytes，吞吐量单位 Mops/s。','', '| 条数 | 实现 | 通过 | 扣锁 heap MiB | 扣锁 heap BPK | 实测 RSS BPK | 插入 Mops/s | 查询 Mops/s |','|---:|---|---:|---:|---:|---:|---:|---:|']
for size in m['sizes']:
    for typ in m['hashmaps']:
        jobs=[j for j in m['jobs'] if j['dataset_size']==size and j['hashmap_type']==typ and j['status']=='passed']
        fields=['index_heap_excluding_locks_bytes_estimate','heap_bpk_excluding_locks_bytes_per_key','rss_bpk_bytes_per_key','insert_throughput_ops_s','read_throughput_ops_s']
        vals=[statistics.median(float(j['csv_row'][f]) for j in jobs) for f in fields] if jobs else [float('nan')]*5
        vals[0]/=2**20; vals[3]/=1e6; vals[4]/=1e6
        text.append(f'| {size:,} | {names[typ]} | {len(jobs)}/3 | '+' | '.join(f'{v:.3f}' for v in vals)+' |')
text+=['','共享池保持固定逻辑桶数量，增长时追加物理页。源码中查询沿桶内槽链遍历；随着数据量增加链变长，可能是查询吞吐量下降的重要原因。本轮未做算法优化或额外剖析。','','## 共享池预算诊断','','| 请求 N | 已存记录中位数 | 拒绝数中位数 | 当前账本 MiB | 历史峰值账本 MiB |','|---:|---:|---:|---:|---:|']
for size in m['sizes']:
    rows=[j['budget_csv_row'] for j in m['jobs'] if j['dataset_size']==size and j['hashmap_type']=='sharehash_pagepool']
    vals=[statistics.median(float(r[f]) for r in rows) for f in ['live_keys_main','insert_rejections','current_live_bytes','peak_live_bytes']]
    text.append(f'| {size:,} | {vals[0]:,.0f} | {vals[1]:,.0f} | {vals[2]/2**20:.3f} | {vals[3]/2**20:.3f} |')
text+=['','预算 CSV 已校验成功/拒绝数、账本/快照一致性、物理与线性上限及比率。历史峰值保持含锁口径。','', '## 产物与复现','', '- [summary.csv](summary.csv)、[memory_summary.csv](memory_summary.csv)：中位数及通过数。标准汇总会保留失败数值，需检查通过数。','- [benchmark_results.csv](benchmark_results.csv)、[memory_results.csv](memory_results.csv)：逐次原始测量。','- [space_budget_results.csv](space_budget_results.csv)：共享池实际配置与预算诊断。','- [run_manifest.json](run_manifest.json)：运行参数、源码与二进制校验和、逐次状态、命令及校验。','- [validation.json](validation.json)、[logs/](logs/)：校验结果和逐次日志。','','在 orishare 目录执行：','','```sh','cmake --build build-single-thread --target benchmark -j4','python3 benchmark/run_single_thread.py --binary build-single-thread/benchmark','```']
(p/'RESULTS.md').write_text('\n'.join(text)+'\n')
(p/'validation.json').write_text(json.dumps({'status':m['status'],'expected':m['expected_job_count'],'passed':m['passed_job_count'],'verified_csv_sha256':checks,'budget_diagnostics_valid':True},indent=2)+'\n')
print('\n'.join(text))
