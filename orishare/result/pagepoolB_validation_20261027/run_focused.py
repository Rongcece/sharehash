#!/usr/bin/env python3
"""Run the focused, value-checking B vs subtable benchmark in fresh processes."""
import csv
import hashlib
import json
import os
from pathlib import Path
import subprocess
from datetime import datetime, timezone

ROOT=Path(__file__).resolve().parents[2]
OUT=Path(__file__).resolve().parent
BINARY=Path('/tmp/pagepoolB_benchmark')
FIELDS='type,n,seed,inserted,hits,wrong_values,insert_seconds,read_seconds,insert_ops_s,read_ops_s,reported_bytes,heap_bytes,lock_bytes,heap_bpk_excluding_locks,index_buckets,page_grows'.split(',')
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def now(): return datetime.now(timezone.utc).isoformat()
def main():
    target=OUT/'focused_results.csv'
    if target.exists(): raise SystemExit('Refusing to overwrite '+str(target))
    allowed=sorted(os.sched_getaffinity(0)); cpu=allowed[0]
    manifest={'started_utc':now(),'cpu':cpu,'allowed_cpus':allowed,'binary_sha256':digest(BINARY),
              'flags':'-std=gnu++14 -pg -g -O1 -fopenmp',
              'query_workload':'get, hit count and returned-value validation inside timing for BOTH implementations; differs from previous count-only suite',
              'sources':{str(p.relative_to(ROOT)):digest(p) for p in [ROOT/'include/ShareHash_pagepoolB.h',ROOT/'include/ShareHash_subtable.h',ROOT/'benchmark/pagepoolB_benchmark.cpp']},'jobs':[]}
    os.sched_setaffinity(0,{cpu})
    env=dict(os.environ,OMP_NUM_THREADS='1',OMP_DYNAMIC='FALSE',GMON_OUT_PREFIX=str(OUT/'gmon'))
    try:
        with target.open('x',newline='') as stream:
            writer=csv.writer(stream);writer.writerow(FIELDS)
            for n in [100000,1000000,5000000]:
                for seed in [20261025,20261026,20261027]:
                    for typ in ['subtable','pagepoolB']:
                        command=[str(BINARY),typ,str(n),str(seed)]
                        result=subprocess.run(command,cwd=str(OUT),env=env,capture_output=True,text=True,timeout=120)
                        (OUT/('{}_{}_{}.stderr.log'.format(typ,n,seed))).write_text(result.stderr)
                        row=next(csv.reader([result.stdout.strip()]))
                        assert result.returncode==0 and len(row)==len(FIELDS),(command,result.stdout,result.stderr)
                        assert row[:6]==[typ,str(n),str(seed),str(n),str(n),'0'],row
                        writer.writerow(row);stream.flush()
                        manifest['jobs'].append({'command':command,'exit_code':result.returncode,'row':dict(zip(FIELDS,row))})
                        print(typ,n,seed,'passed',flush=True)
    finally:
        os.sched_setaffinity(0,allowed)
        manifest['finished_utc']=now()
        manifest['status']='passed' if len(manifest['jobs'])==18 else 'incomplete'
        manifest['csv_sha256']=digest(target)
        (OUT/'focused_manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
if __name__=='__main__': main()
