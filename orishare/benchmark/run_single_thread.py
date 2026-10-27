#!/usr/bin/env python3
"""Run six hash benchmarks (including three independent ShareHash variants) in separate single-thread processes.

Example (from the project root):
    python3 benchmark/run_single_thread.py --binary build/benchmark

Each invocation uses a fresh result directory. BPK columns use bytes per key.
The benchmark owns benchmark_results.csv; this script adds logs, a manifest,
and summary.csv without changing benchmark workloads or hashmap settings.
"""

import argparse
import csv
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone


PROJECT_ROOT = Path(__file__).resolve().parents[1]
HASHMAPS = ("sharehash", "sharehash_subtable", "sharehash_pagepool", "unorderedmap", "cuckoohash", "sparsehash")
CSV_FIELDS = (
    "timestamp_utc", "hashmap_type", "dataset_size", "thread_count", "seed",
    "insert_successes", "lookup_hits", "insert_seconds", "read_seconds",
    "insert_throughput_ops_s", "read_throughput_ops_s", "reported_memory_bytes",
    "reported_bpk_bytes_per_key", "index_heap_bytes_estimate",
    "heap_bpk_bytes_per_key", "index_rss_bytes_estimate", "rss_bpk_bytes_per_key",
    "process_rss_bytes", "peak_process_rss_bytes", "dataset_payload_bytes",
    "input_heap_bytes_estimate", "input_rss_bytes_estimate", "rss_before_init_bytes",
    "rss_after_init_bytes", "rss_before_insert_bytes", "heap_before_init_bytes",
    "heap_after_init_bytes", "heap_before_insert_bytes", "heap_after_workload_bytes",
    "lock_memory_bytes", "reported_memory_excluding_locks_bytes",
    "reported_bpk_excluding_locks_bytes_per_key", "index_heap_excluding_locks_bytes_estimate",
    "heap_bpk_excluding_locks_bytes_per_key",
)
METRIC_FIELDS = CSV_FIELDS[7:]
MEMORY_CSV_FIELDS = (
    "timestamp_utc", "phase", "hashmap_type", "dataset_size", "thread_count", "seed",
    "insert_successes", "insert_seconds", "insert_throughput_ops_s", "reported_memory_bytes",
    "reported_bpk_bytes_per_key", "index_heap_bytes_estimate", "heap_bpk_bytes_per_key",
    "index_rss_bytes_estimate", "rss_bpk_bytes_per_key", "process_rss_bytes",
    "peak_process_rss_bytes", "dataset_payload_bytes", "input_heap_bytes_estimate",
    "input_rss_bytes_estimate", "rss_before_init_bytes", "rss_after_init_bytes",
    "rss_before_insert_bytes", "heap_before_init_bytes", "heap_after_init_bytes",
    "heap_before_insert_bytes", "heap_after_insert_bytes",
    "lock_memory_bytes", "reported_memory_excluding_locks_bytes",
    "reported_bpk_excluding_locks_bytes_per_key", "index_heap_excluding_locks_bytes_estimate",
    "heap_bpk_excluding_locks_bytes_per_key",
)
MEMORY_METRIC_FIELDS = MEMORY_CSV_FIELDS[6:]
BUDGET_CSV_FIELDS = (
    "timestamp_utc", "hashmap_type", "dataset_size", "thread_count", "seed", "insert_successes",
    "insert_rejections", "insert_seconds", "successful_insert_throughput_ops_s",
    "insert_acceptance_fraction", "slot_capacity", "subtable_count", "physical_limit_bytes",
    "a_stable_factor", "a_migration_factor", "additive_bytes", "min_load_factor", "max_load_factor",
    "current_live_bytes", "peak_live_bytes", "live_keys_main", "live_keys_fallback",
    "allowed_stable_bytes", "allowed_migration_bytes", "post_insert_load_factor",
    "lock_memory_bytes", "reported_bpk_per_stored_key_excluding_locks",
)
UINT64_MAX = (1 << 64) - 1


def utc_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def positive_integer(value):
    number = int(value)
    if number < 1:
        raise argparse.ArgumentTypeError("must be a positive integer")
    return number


def uint64(value):
    number = int(value)
    if not 0 <= number <= UINT64_MAX:
        raise argparse.ArgumentTypeError("must be between 0 and 2**64 - 1")
    return number


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def write_manifest(path, manifest):
    temporary = path.with_suffix(".json.tmp")
    temporary.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n")
    temporary.replace(path)


def read_results(path, fields=CSV_FIELDS):
    if not path.exists():
        return []
    with path.open(newline="") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != list(fields):
            raise ValueError(path.name + " header does not match the expected schema")
        rows = list(reader)
        if any(None in row or any(value is None for value in row.values()) for row in rows):
            raise ValueError(path.name + " contains an incomplete or oversized row")
        return rows


def validate_lock_accounting(row, hashmap, size):
    errors = []
    try:
        locks = int(row["lock_memory_bytes"])
        if locks < 0 or (hashmap in ("sharehash", "sharehash_subtable", "sharehash_pagepool") and locks == 0):
            errors.append("invalid ShareHash lock-object byte count")
        if hashmap not in ("sharehash", "sharehash_subtable", "sharehash_pagepool") and locks != 0:
            errors.append("unexpected lock deduction for another hashmap")
        for source, adjusted, bpk in (
            ("reported_memory_bytes", "reported_memory_excluding_locks_bytes", "reported_bpk_excluding_locks_bytes_per_key"),
            ("index_heap_bytes_estimate", "index_heap_excluding_locks_bytes_estimate", "heap_bpk_excluding_locks_bytes_per_key"),
        ):
            expected = int(row[source]) - locks
            if int(row[adjusted]) != expected:
                errors.append(adjusted + " does not equal raw bytes minus lock objects")
            if not math.isclose(float(row[bpk]), expected / size, rel_tol=1e-12, abs_tol=1e-12):
                errors.append(bpk + " does not equal adjusted bytes divided by requested keys")
    except (ValueError, TypeError, KeyError):
        errors.append("lock-accounting fields are invalid")
    return errors


def validate_row(row, hashmap, size, seed):
    errors = []
    expected = {"hashmap_type": hashmap, "dataset_size": str(size),
                "thread_count": "1", "seed": str(seed)}
    for field, value in expected.items():
        if row[field] != value:
            errors.append("{}: expected {}, got {}".format(field, value, row[field]))
    for field in CSV_FIELDS[2:]:
        try:
            if not math.isfinite(float(row[field])):
                errors.append(field + " is not finite")
        except (ValueError, TypeError):
            errors.append(field + " is not numeric")
    try:
        datetime.fromisoformat(row["timestamp_utc"].replace("Z", "+00:00"))
    except ValueError:
        errors.append("timestamp_utc is not an ISO timestamp")
    for field in ("insert_successes", "lookup_hits"):
        try:
            if int(row[field]) != size:
                errors.append("{}: expected {}, got {}".format(field, size, row[field]))
        except ValueError:
            errors.append(field + " is not an integer")
    for field in ("insert_seconds", "read_seconds", "insert_throughput_ops_s",
                  "read_throughput_ops_s"):
        try:
            if float(row[field]) <= 0:
                errors.append(field + " must be positive")
        except ValueError:
            pass  # Already reported by the numeric-field validation above.
    errors.extend(validate_lock_accounting(row, hashmap, size))
    return errors


def validate_memory_row(row, hashmap, size, seed):
    errors = []
    expected = {"phase": "after_insert", "hashmap_type": hashmap,
                "dataset_size": str(size), "thread_count": "1", "seed": str(seed)}
    for field, value in expected.items():
        if row[field] != value:
            errors.append("{}: expected {}, got {}".format(field, value, row[field]))
    for field in MEMORY_CSV_FIELDS[3:]:
        try:
            if not math.isfinite(float(row[field])):
                errors.append(field + " is not finite")
        except (ValueError, TypeError):
            errors.append(field + " is not numeric")
    try:
        datetime.fromisoformat(row["timestamp_utc"].replace("Z", "+00:00"))
    except ValueError:
        errors.append("timestamp_utc is not an ISO timestamp")
    try:
        if int(row["insert_successes"]) != size:
            errors.append("insert_successes: expected {}, got {}".format(size, row["insert_successes"]))
    except ValueError:
        errors.append("insert_successes is not an integer")
    for field in ("insert_seconds", "insert_throughput_ops_s"):
        try:
            if float(row[field]) <= 0:
                errors.append(field + " must be positive")
        except ValueError:
            pass
    errors.extend(validate_lock_accounting(row, hashmap, size))
    return errors


def validate_budget_row(row, job):
    errors = []
    try:
        n = job["dataset_size"]
        for field, expected in (("hashmap_type", "sharehash_pagepool"), ("dataset_size", str(n)),
                                ("thread_count", "1"), ("seed", str(job["seed"]))):
            if row[field] != expected:
                errors.append("unexpected budget " + field)
        if any(not math.isfinite(float(row[field])) for field in BUDGET_CSV_FIELDS[2:]):
            errors.append("nonfinite budget numeric field")
        actual = int(row["live_keys_main"])
        current = int(row["current_live_bytes"])
        peak = int(row["peak_live_bytes"])
        limit = int(row["physical_limit_bytes"])
        locks = int(row["lock_memory_bytes"])
        slots = int(row["slot_capacity"])
        if actual != int(row["insert_successes"]) or int(row["insert_rejections"]) != n - actual:
            errors.append("budget success/rejection accounting mismatch")
        if int(row["live_keys_fallback"]) != 0 or int(row["subtable_count"]) != 32:
            errors.append("unexpected V2-A fallback or subtable count")
        if not (0 <= current <= peak <= limit):
            errors.append("V2-A peak exceeds physical storage-byte limit")
        for factor, recorded, measured in (("a_stable_factor", "allowed_stable_bytes", current),
                                            ("a_migration_factor", "allowed_migration_bytes", peak)):
            allowed = min(limit, int(float(row[factor]) * actual) + int(row["additive_bytes"]))
            if int(row[recorded]) != allowed or measured > allowed:
                errors.append("V2-A affine budget mismatch: " + factor)
        memory = job.get("memory_csv_row", {})
        if (actual != int(memory.get("insert_successes", -1))
                or current != int(memory.get("reported_memory_bytes", -1))
                or locks != int(memory.get("lock_memory_bytes", -1))):
            errors.append("budget diagnostics disagree with insertion snapshot")
        expected = {
            "successful_insert_throughput_ops_s": actual / float(row["insert_seconds"]),
            "insert_acceptance_fraction": actual / n,
            "post_insert_load_factor": actual / slots,
            "reported_bpk_per_stored_key_excluding_locks": (current - locks) / actual if actual else 0,
        }
        for field, value in expected.items():
            if not math.isclose(float(row[field]), value, rel_tol=1e-12, abs_tol=1e-12):
                errors.append("budget ratio mismatch: " + field)
    except (ValueError, TypeError, KeyError, ZeroDivisionError):
        errors.append("invalid budget diagnostics fields")
    return errors


def attach_budget_diagnostics(path, jobs):
    targets = [job for job in jobs if job["hashmap_type"] == "sharehash_pagepool"]
    try:
        rows = read_results(path, BUDGET_CSV_FIELDS)
        global_errors = [] if len(rows) == len(targets) else ["unexpected budget CSV row count"]
    except (OSError, ValueError, csv.Error) as error:
        rows, global_errors = [], ["invalid budget CSV: " + str(error)]
    for job in targets:
        matches = [(i, row) for i, row in enumerate(rows, 1)
                   if row["dataset_size"] == str(job["dataset_size"]) and row["seed"] == str(job["seed"])
                   and row["hashmap_type"] == job["hashmap_type"]]
        errors = list(global_errors)
        if len(matches) != 1:
            errors.append("expected exactly one budget diagnostic row")
        else:
            number, row = matches[0]
            job["budget_csv_row"] = row
            job["budget_csv_row_number"] = number
            errors.extend(validate_budget_row(row, job))
        job["budget_validation_errors"] = errors
        if errors:
            job["validation_errors"].extend("budget: " + error for error in errors)
            job["status"] = "failed"
    return global_errors


def machine_information():
    cpu_model = "unknown"
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.exists():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                cpu_model = line.split(":", 1)[1].strip()
                break
    return {
        "hostname": platform.node(), "kernel": platform.release(),
        "platform": platform.platform(), "machine": platform.machine(),
        "cpu_model": cpu_model, "logical_cpu_count": os.cpu_count(),
        "python": sys.version, "libc": dict(zip(("name", "version"), platform.libc_ver())),
    }


def build_information(binary):
    information = {}
    flags_path = binary.parent / "CMakeFiles/benchmark.dir/flags.make"
    if flags_path.is_file():
        information["benchmark_flags_make"] = {
            "path": str(flags_path), "sha256": sha256(flags_path),
            "content": flags_path.read_text(errors="replace"),
        }
    return information


def source_hashes():
    paths = [PROJECT_ROOT / "CMakeLists.txt", PROJECT_ROOT / "main.cpp", Path(__file__).resolve()]
    for directory in (PROJECT_ROOT / "include", PROJECT_ROOT / "benchmark",
                      PROJECT_ROOT / "libcuckoo/libcuckoo"):
        paths.extend(path for path in directory.rglob("*")
                     if path.is_file() and path.suffix in (".h", ".hh", ".hpp", ".cpp"))
    return {str(path.relative_to(PROJECT_ROOT)): sha256(path)
            for path in sorted(set(paths))}


def write_summary(path, jobs, hashmaps, sizes):
    fields = ["hashmap_type", "dataset_size", "thread_count", "count",
              "correctness_pass_count", "min_insert_successes", "min_lookup_hits"]
    fields.extend("median_" + field for field in METRIC_FIELDS)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for size in sizes:
            for hashmap in hashmaps:
                candidates = [job for job in jobs if job["hashmap_type"] == hashmap
                              and job["dataset_size"] == size and "csv_row" in job]
                # Include structurally valid numeric rows even when correctness failed;
                # the success/hit minima and pass count make those failures visible.
                usable = []
                for job in candidates:
                    try:
                        row = job["csv_row"]
                        if all(math.isfinite(float(row[field])) for field in CSV_FIELDS[2:]):
                            int(row["insert_successes"])
                            int(row["lookup_hits"])
                            usable.append(job)
                    except (ValueError, TypeError):
                        pass
                record = {
                    "hashmap_type": hashmap, "dataset_size": size, "thread_count": 1,
                    "count": len(usable),
                    "correctness_pass_count": sum(job["status"] == "passed" for job in usable),
                    "min_insert_successes": min((int(job["csv_row"]["insert_successes"])
                                                 for job in usable), default=""),
                    "min_lookup_hits": min((int(job["csv_row"]["lookup_hits"])
                                           for job in usable), default=""),
                }
                for field in METRIC_FIELDS:
                    record["median_" + field] = (statistics.median(
                        float(job["csv_row"][field]) for job in usable) if usable else "")
                writer.writerow(record)


def write_memory_summary(path, jobs, hashmaps, sizes):
    fields = ["hashmap_type", "dataset_size", "thread_count", "sample_count",
              "insert_success_check_count", "completed_read_check_count"]
    fields.extend("median_" + field for field in MEMORY_METRIC_FIELDS)
    with path.open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for size in sizes:
            for hashmap in hashmaps:
                group = [job for job in jobs if job["hashmap_type"] == hashmap
                         and job["dataset_size"] == size]
                usable = []
                for job in group:
                    if "memory_csv_row" not in job:
                        continue
                    try:
                        row = job["memory_csv_row"]
                        if (row["phase"] == "after_insert" and row["hashmap_type"] == hashmap
                                and row["dataset_size"] == str(size) and row["thread_count"] == "1"
                                and row["seed"] == str(job["seed"])
                                and all(math.isfinite(float(row[field])) for field in MEMORY_CSV_FIELDS[3:])):
                            usable.append(job)
                    except (ValueError, TypeError):
                        pass
                completed_reads = sum(
                    job["exit_code"] == 0 and "csv_row" in job
                    and not validate_row(job["csv_row"], hashmap, size, job["seed"])
                    for job in group)
                record = {
                    "hashmap_type": hashmap, "dataset_size": size, "thread_count": 1,
                    "sample_count": len(usable),
                    "insert_success_check_count": sum(
                        not job.get("memory_validation_errors") for job in usable),
                    "completed_read_check_count": completed_reads,
                }
                for field in MEMORY_METRIC_FIELDS:
                    record["median_" + field] = statistics.median(
                        float(job["memory_csv_row"][field]) for job in usable) if usable else ""
                writer.writerow(record)


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--binary", type=Path, default=PROJECT_ROOT / "build/benchmark")
    parser.add_argument("--result-dir", type=Path,
                        help="fresh output directory (default: result/single_thread_<UTC time>)")
    parser.add_argument("--sizes", type=positive_integer, nargs="+",
                        default=[100000, 1000000, 5000000])
    parser.add_argument("--repeats", type=positive_integer, default=3)
    parser.add_argument("--seed", type=uint64, default=20261025,
                        help="repeat r uses (seed + r) modulo 2**64 for every hashmap")
    parser.add_argument("--hashmaps", choices=HASHMAPS, nargs="+", default=list(HASHMAPS))
    parser.add_argument("--timeout", type=positive_integer, default=300,
                        help="timeout in seconds for each subprocess")
    parser.add_argument("--no-pin-cpu", action="store_true",
                        help="do not bind the runner and its children to the first allowed CPU")
    parser.add_argument("--binary-wait-seconds", type=float, default=30,
                        help="wait up to this many seconds for the compiled binary (default: 30)")
    args = parser.parse_args()
    if args.binary_wait_seconds < 0 or not math.isfinite(args.binary_wait_seconds):
        parser.error("--binary-wait-seconds must be finite and nonnegative")
    if any(size > 2147483647 for size in args.sizes):
        parser.error("--sizes must fit the benchmark's signed int dataset-size")
    if len(set(args.sizes)) != len(args.sizes) or len(set(args.hashmaps)) != len(args.hashmaps):
        parser.error("duplicate sizes or hashmaps are not allowed")
    return args


def main():
    args = parse_arguments()
    binary = args.binary.expanduser().resolve()
    deadline = time.monotonic() + args.binary_wait_seconds
    while not binary.is_file() and time.monotonic() < deadline:
        time.sleep(min(0.2, max(0, deadline - time.monotonic())))
    if not binary.is_file() or not os.access(binary, os.X_OK):
        print("Executable benchmark binary not found: " + str(binary), file=sys.stderr)
        return 2

    default_name = "single_thread_" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S_%fZ")
    result_dir = (args.result_dir or PROJECT_ROOT / "result" / default_name).expanduser().resolve()
    if result_dir.exists() and (not result_dir.is_dir() or any(result_dir.iterdir())):
        print("Result directory must be new or empty: " + str(result_dir), file=sys.stderr)
        return 2

    original_affinity = sorted(os.sched_getaffinity(0)) if hasattr(os, "sched_getaffinity") else None
    pinned_cpu = None
    if not args.no_pin_cpu:
        if not original_affinity or not hasattr(os, "sched_setaffinity"):
            print("CPU affinity unavailable; use --no-pin-cpu to proceed.", file=sys.stderr)
            return 2
        pinned_cpu = original_affinity[0]
        try:
            os.sched_setaffinity(0, {pinned_cpu})
        except OSError as error:
            print("Cannot bind to CPU {}: {}".format(pinned_cpu, error), file=sys.stderr)
            return 2

    result_dir.mkdir(parents=True, exist_ok=True)
    logs_dir = result_dir / "logs"
    logs_dir.mkdir()
    csv_path = result_dir / "benchmark_results.csv"
    memory_csv_path = result_dir / "memory_results.csv"
    manifest_path = result_dir / "run_manifest.json"
    environment = os.environ.copy()
    environment.update({"OMP_NUM_THREADS": "1", "OMP_DYNAMIC": "FALSE",
                        "GMON_OUT_PREFIX": str(logs_dir / "gmon")})
    manifest = {
        "started_utc": utc_now(), "finished_utc": None, "status": "running",
        "project_root": str(PROJECT_ROOT), "binary": str(binary),
        "binary_sha256": sha256(binary), "source_sha256": source_hashes(),
        "build": build_information(binary),
        "machine": machine_information(), "allowed_cpus": original_affinity,
        "pinned_cpu": pinned_cpu, "sizes": args.sizes, "hashmaps": args.hashmaps,
        "repeats": args.repeats, "thread_count": 1, "base_seed": args.seed,
        "seed_strategy": "(base_seed + repeat) modulo 2**64; repeat starts at 0",
        "timeout_seconds": args.timeout, "working_directory": str(PROJECT_ROOT),
        "environment_overrides": {field: environment[field] for field in
                                  ("OMP_NUM_THREADS", "OMP_DYNAMIC", "GMON_OUT_PREFIX")},
        "csv_fields": list(CSV_FIELDS), "memory_csv_fields": list(MEMORY_CSV_FIELDS),
        "memory_phase": "after_insert", "bpk_unit": "bytes/key",
        "primary_memory_metric": "heap_bpk_excluding_locks_bytes_per_key",
        "lock_accounting": {
            "policy": "For all three ShareHash variants, subtract lock-object bytes from raw reported and estimated heap bytes; retain raw columns for auditing.",
            "sharehash": "(slot capacity + 1 table lock) * sizeof(SpinLock)",
            "sharehash_subtable": "(total slot capacity + T table locks) * sizeof(SpinLock)",
            "sharehash_pagepool": "(slot capacity + T table locks + 1 budget lock) * sizeof(SpinLock)",
            "other_hashmaps": "No deduction; zero does not imply absence of internal locks.",
            "retained": "Padding, pointer fields and allocator metadata remain; RSS and timing are actual measurements with locks executing.",
        },
        "budget_csv_fields": list(BUDGET_CSV_FIELDS),
        "budget_diagnostics_policy": "PagePool B after-insert ledger includes locks; finite physical cap is not configured (size_t max). Partial workloads are failures, with stored-key ratios retained only as diagnostics.",
        "hashmap_parameters": {
            "sharehash": {"variant": "current repaired single table", "initial_total_buckets": 5000, "initial_total_slots": 20000},
            "sharehash_subtable": {"T": 16, "T_policy": "auto: floor_pow2(min(256,max(1,initial_total_buckets//256)))", "max_load_factor": 0.8, "initial_total_buckets": 5000, "initial_total_slots": 20000},
            "sharehash_pagepool": {"T": 32, "max_load_factor": 0.8, "min_load_factor": 0.75, "a_stable_factor": 48, "a_migration_factor": 48, "additive_policy": "automatic object plus initial owned storage", "physical_limit_bytes": UINT64_MAX, "physical_limit_policy": "default size_t max, no finite external cap", "initial_total_buckets": 5000, "initial_total_slots": 20480, "page_slots": 128, "fallback": "none", "variant": "PagePool B; stable record pages and linear-hash handle splits", "source_header": "include/ShareHash_pagepoolB.h", "directory": "four-level radix", "min_load_policy": "global added slots beyond initial capacity", "record_migration": "none"},
        },
        "summary_policy": "Median numeric metrics from numeric CSV rows, including rows with "
                          "correctness failures; inspect correctness_pass_count and success/hit minima.",
        "memory_summary_policy": "Median after-insert numeric metrics, including samples from "
                                 "processes that later fail; inspect insert_success_check_count and "
                                 "completed_read_check_count before interpreting samples as valid indexes.",
        "jobs": [],
    }
    write_manifest(manifest_path, manifest)
    total_jobs = len(args.sizes) * args.repeats * len(args.hashmaps)
    interrupted = False
    try:
        for size in args.sizes:
            for repeat in range(args.repeats):
                seed = (args.seed + repeat) & UINT64_MAX
                for hashmap in args.hashmaps:
                    job_number = len(manifest["jobs"]) + 1
                    name = "{:03d}_{}_n{}_repeat{}".format(job_number, hashmap, size, repeat)
                    stdout_path = logs_dir / (name + ".stdout.log")
                    stderr_path = logs_dir / (name + ".stderr.log")
                    command = [str(binary), "--result-dir", str(result_dir), "--seed", str(seed),
                               "-t", hashmap, "-d", str(size), "-p", "1"]
                    job = {
                        "job_number": job_number, "hashmap_type": hashmap, "dataset_size": size,
                        "repeat": repeat, "seed": seed, "command": command,
                        "stdout_log": str(stdout_path.relative_to(result_dir)),
                        "stderr_log": str(stderr_path.relative_to(result_dir)),
                        "started_utc": utc_now(), "exit_code": None, "timed_out": False,
                        "status": "running", "validation_errors": [], "memory_validation_errors": [],
                    }
                    manifest["jobs"].append(job)
                    print("[{}/{}] {} n={} repeat={} seed={}".format(
                        job_number, total_jobs, hashmap, size, repeat, seed), flush=True)
                    try:
                        before = read_results(csv_path)
                    except (OSError, ValueError, csv.Error) as error:
                        before = None
                        job["validation_errors"].append("CSV before subprocess is invalid: " + str(error))
                    try:
                        memory_before = read_results(memory_csv_path, MEMORY_CSV_FIELDS)
                    except (OSError, ValueError, csv.Error) as error:
                        memory_before = None
                        job["memory_validation_errors"].append(
                            "memory CSV before subprocess is invalid: " + str(error))
                    write_manifest(manifest_path, manifest)
                    started = time.monotonic()
                    try:
                        with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
                            completed = subprocess.run(command, cwd=PROJECT_ROOT, env=environment,
                                                       stdout=stdout, stderr=stderr, timeout=args.timeout)
                            job["exit_code"] = completed.returncode
                    except subprocess.TimeoutExpired:
                        job["timed_out"] = True
                        job["exit_code"] = 124
                        job["validation_errors"].append("subprocess timed out")
                    except OSError as error:
                        job["validation_errors"].append("cannot execute subprocess: " + str(error))
                    job["wall_seconds"] = time.monotonic() - started
                    job["finished_utc"] = utc_now()
                    if job["exit_code"] != 0:
                        job["validation_errors"].append("nonzero subprocess exit code: {}".format(
                            job["exit_code"]))
                    try:
                        after = read_results(csv_path)
                        job["csv_rows_before"] = len(before) if before is not None else None
                        job["csv_rows_after"] = len(after)
                        if before is not None and after[:len(before)] != before:
                            job["validation_errors"].append("previous CSV rows were modified")
                        if before is None or len(after) != len(before) + 1:
                            job["validation_errors"].append("expected exactly one appended CSV row")
                        else:
                            row = after[-1]
                            job["csv_row"] = row
                            job["csv_row_number"] = len(after)
                            job["validation_errors"].extend(validate_row(row, hashmap, size, seed))
                    except (OSError, ValueError, csv.Error) as error:
                        job["validation_errors"].append("CSV validation failed: " + str(error))
                    try:
                        memory_after = read_results(memory_csv_path, MEMORY_CSV_FIELDS)
                        job["memory_csv_rows_before"] = (len(memory_before)
                                                         if memory_before is not None else None)
                        job["memory_csv_rows_after"] = len(memory_after)
                        if memory_before is not None and memory_after[:len(memory_before)] != memory_before:
                            job["memory_validation_errors"].append("previous memory CSV rows were modified")
                        if memory_before is None or len(memory_after) != len(memory_before) + 1:
                            job["memory_validation_errors"].append("expected exactly one appended memory CSV row")
                        else:
                            memory_row = memory_after[-1]
                            job["memory_csv_row"] = memory_row
                            job["memory_csv_row_number"] = len(memory_after)
                            job["memory_validation_errors"].extend(
                                validate_memory_row(memory_row, hashmap, size, seed))
                    except (OSError, ValueError, csv.Error) as error:
                        job["memory_validation_errors"].append("memory CSV validation failed: " + str(error))
                    job["validation_errors"].extend("memory: " + error
                                                    for error in job["memory_validation_errors"])
                    job["status"] = "failed" if job["validation_errors"] else "passed"
                    print("  {}{}".format(job["status"], ": " + "; ".join(
                        job["validation_errors"]) if job["validation_errors"] else ""), flush=True)
                    write_manifest(manifest_path, manifest)
    except KeyboardInterrupt:
        interrupted = True
        print("Interrupted; saving completed results.", file=sys.stderr)
    except (OSError, ValueError, csv.Error) as error:
        manifest["runner_error"] = str(error)
        print("Runner error: " + str(error), file=sys.stderr)
    finally:
        if pinned_cpu is not None:
            os.sched_setaffinity(0, original_affinity)
        manifest["budget_diagnostic_errors"] = attach_budget_diagnostics(
            result_dir / "space_budget_results.csv", manifest["jobs"])
        write_summary(result_dir / "summary.csv", manifest["jobs"], args.hashmaps, args.sizes)
        write_memory_summary(result_dir / "memory_summary.csv", manifest["jobs"], args.hashmaps, args.sizes)
        passed = sum(job["status"] == "passed" for job in manifest["jobs"])
        manifest["finished_utc"] = utc_now()
        manifest["expected_job_count"] = total_jobs
        manifest["passed_job_count"] = passed
        manifest["failed_or_incomplete_job_count"] = total_jobs - passed
        manifest["status"] = "passed" if passed == total_jobs else "failed"
        manifest["interrupted"] = interrupted
        if csv_path.exists():
            manifest["benchmark_results_sha256"] = sha256(csv_path)
        if memory_csv_path.exists():
            manifest["memory_results_sha256"] = sha256(memory_csv_path)
        if (result_dir / "space_budget_results.csv").exists():
            manifest["space_budget_results_sha256"] = sha256(result_dir / "space_budget_results.csv")
        manifest["summary_sha256"] = sha256(result_dir / "summary.csv")
        manifest["memory_summary_sha256"] = sha256(result_dir / "memory_summary.csv")
        write_manifest(manifest_path, manifest)
    print("Results: " + str(result_dir), flush=True)
    print("Passed {}/{} jobs".format(manifest["passed_job_count"], total_jobs), flush=True)
    return 0 if manifest["status"] == "passed" else (130 if interrupted else 1)


if __name__ == "__main__":
    sys.exit(main())
