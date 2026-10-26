#ifndef SHAREHASH_RESULT_WRITER_H
#define SHAREHASH_RESULT_WRITER_H

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <malloc.h>
#include <stdexcept>
#include <string>
#include <sys/resource.h>
#include <sys/stat.h>

#ifndef SHAREHASH_RESULT_DIR
#define SHAREHASH_RESULT_DIR "result"
#endif

namespace benchmark_results {

struct MemorySnapshot {
    long long heap_bytes;
    long long rss_bytes;
};

// glibc heap blocks in use plus live mmap allocations; not total process memory.
inline MemorySnapshot sample_memory() {
#if __GLIBC_PREREQ(2, 33)
    const auto info = mallinfo2();
#else
    // The older API uses signed 32-bit counters. Reject overflow rather than
    // saving a misleading value; the default single-thread suite is below 2 GiB.
    const auto info = mallinfo();
    if (info.uordblks < 0 || info.hblkhd < 0) {
        throw std::runtime_error("mallinfo overflow; use glibc >= 2.33 for this scale");
    }
#endif
    const long long heap = static_cast<long long>(info.uordblks) + info.hblkhd;
    FILE* file = std::fopen("/proc/self/smaps_rollup", "r");
    if (!file) {
        throw std::runtime_error("Cannot read /proc/self/smaps_rollup for RSS");
    }
    char line[256];
    long long rss_kib = -1;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::sscanf(line, "Rss: %lld kB", &rss_kib) == 1) {
            break;
        }
    }
    std::fclose(file);
    if (rss_kib < 0) {
        throw std::runtime_error("RSS is missing from /proc/self/smaps_rollup");
    }
    return {heap, rss_kib * 1024};
}

inline long long peak_rss_bytes() {
    struct rusage usage {};
    if (getrusage(RUSAGE_SELF, &usage) != 0) {
        throw std::runtime_error("Cannot read peak RSS");
    }
    return static_cast<long long>(usage.ru_maxrss) * 1024;
}

struct Result {
    std::string hashmap_type;
    int dataset_size;
    int thread_count;
    uint64_t seed;
    long long insert_successes;
    long long lookup_hits;
    double insert_seconds;
    double read_seconds;
    long long reported_memory_bytes;
    long long dataset_payload_bytes;
    MemorySnapshot before_init;
    MemorySnapshot after_init;
    MemorySnapshot before_insert;
    MemorySnapshot after_workload;
};

inline void ensure_directory(const std::string& directory) {
    if (directory.empty()) {
        throw std::runtime_error("Result directory must not be empty");
    }
    for (size_t i = 1; i <= directory.size(); ++i) {
        if (i != directory.size() && directory[i] != '/') {
            continue;
        }
        const std::string part = directory.substr(0, i);
        if (::mkdir(part.c_str(), 0775) != 0 && errno != EEXIST) {
            throw std::runtime_error("Cannot create result directory: " + part);
        }
        struct stat status {};
        if (::stat(part.c_str(), &status) != 0 || !S_ISDIR(status.st_mode)) {
            throw std::runtime_error("Result path is not a directory: " + part);
        }
    }
}

inline std::ofstream open_csv(const std::string& directory, const std::string& filename,
                              const std::string& header) {
    ensure_directory(directory);
    const std::string path = directory + "/" + filename;
    bool needs_header = true;
    {
        std::ifstream previous(path);
        std::string previous_header;
        if (std::getline(previous, previous_header)) {
            if (previous_header != header) {
                throw std::runtime_error("CSV header does not match: " + path);
            }
            needs_header = false;
        }
    }
    std::ofstream output(path, std::ios::app);
    if (!output) {
        throw std::runtime_error("Cannot open result CSV: " + path);
    }
    if (needs_header) {
        output << header << '\n';
    }
    output << std::setprecision(17);
    return output;
}

inline std::string utc_timestamp() {
    const std::time_t now = std::time(nullptr);
    char timestamp[32];
    std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", std::gmtime(&now));
    return timestamp;
}

inline std::string append_csv(const std::string& directory, const Result& result) {
    const std::string path = directory + "/benchmark_results.csv";
    auto output = open_csv(directory, "benchmark_results.csv",
        "timestamp_utc,hashmap_type,dataset_size,thread_count,seed,"
        "insert_successes,lookup_hits,insert_seconds,read_seconds,"
        "insert_throughput_ops_s,read_throughput_ops_s,reported_memory_bytes,"
        "reported_bpk_bytes_per_key,index_heap_bytes_estimate,heap_bpk_bytes_per_key,"
        "index_rss_bytes_estimate,rss_bpk_bytes_per_key,process_rss_bytes,"
        "peak_process_rss_bytes,dataset_payload_bytes,input_heap_bytes_estimate,"
        "input_rss_bytes_estimate,rss_before_init_bytes,rss_after_init_bytes,"
        "rss_before_insert_bytes,heap_before_init_bytes,heap_after_init_bytes,"
        "heap_before_insert_bytes,heap_after_workload_bytes");
    const long long index_heap = result.after_init.heap_bytes - result.before_init.heap_bytes
        + result.after_workload.heap_bytes - result.before_insert.heap_bytes;
    const long long index_rss = result.after_init.rss_bytes - result.before_init.rss_bytes
        + result.after_workload.rss_bytes - result.before_insert.rss_bytes;
    const double keys = result.dataset_size;
    output << utc_timestamp() << ',' << result.hashmap_type << ',' << result.dataset_size << ','
        << result.thread_count << ',' << result.seed << ',' << result.insert_successes << ','
        << result.lookup_hits << ',' << result.insert_seconds << ',' << result.read_seconds << ','
        << keys / result.insert_seconds << ',' << keys / result.read_seconds << ','
        << result.reported_memory_bytes << ',' << result.reported_memory_bytes / keys << ','
        << index_heap << ',' << index_heap / keys << ',' << index_rss << ',' << index_rss / keys << ','
        << result.after_workload.rss_bytes << ',' << peak_rss_bytes() << ','
        << result.dataset_payload_bytes << ','
        << result.before_insert.heap_bytes - result.after_init.heap_bytes << ','
        << result.before_insert.rss_bytes - result.after_init.rss_bytes << ','
        << result.before_init.rss_bytes << ',' << result.after_init.rss_bytes << ','
        << result.before_insert.rss_bytes << ',' << result.before_init.heap_bytes << ','
        << result.after_init.heap_bytes << ',' << result.before_insert.heap_bytes << ','
        << result.after_workload.heap_bytes << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write result CSV: " + path);
    }
    return path;
}

// Persist the insertion checkpoint before querying. This survives a later abort
// in an existing hashmap implementation; it is not a completed query result.
inline std::string append_memory_csv(const std::string& directory, const Result& result) {
    const std::string path = directory + "/memory_results.csv";
    auto output = open_csv(directory, "memory_results.csv",
        "timestamp_utc,phase,hashmap_type,dataset_size,thread_count,seed,insert_successes,"
        "insert_seconds,insert_throughput_ops_s,reported_memory_bytes,"
        "reported_bpk_bytes_per_key,index_heap_bytes_estimate,heap_bpk_bytes_per_key,"
        "index_rss_bytes_estimate,rss_bpk_bytes_per_key,process_rss_bytes,"
        "peak_process_rss_bytes,dataset_payload_bytes,input_heap_bytes_estimate,"
        "input_rss_bytes_estimate,rss_before_init_bytes,rss_after_init_bytes,"
        "rss_before_insert_bytes,heap_before_init_bytes,heap_after_init_bytes,"
        "heap_before_insert_bytes,heap_after_insert_bytes");
    const long long index_heap = result.after_init.heap_bytes - result.before_init.heap_bytes
        + result.after_workload.heap_bytes - result.before_insert.heap_bytes;
    const long long index_rss = result.after_init.rss_bytes - result.before_init.rss_bytes
        + result.after_workload.rss_bytes - result.before_insert.rss_bytes;
    const double keys = result.dataset_size;
    output << utc_timestamp() << ",after_insert," << result.hashmap_type << ',' << result.dataset_size << ','
        << result.thread_count << ',' << result.seed << ',' << result.insert_successes << ','
        << result.insert_seconds << ',' << keys / result.insert_seconds << ','
        << result.reported_memory_bytes << ',' << result.reported_memory_bytes / keys << ','
        << index_heap << ',' << index_heap / keys << ',' << index_rss << ',' << index_rss / keys << ','
        << result.after_workload.rss_bytes << ',' << peak_rss_bytes() << ','
        << result.dataset_payload_bytes << ','
        << result.before_insert.heap_bytes - result.after_init.heap_bytes << ','
        << result.before_insert.rss_bytes - result.after_init.rss_bytes << ','
        << result.before_init.rss_bytes << ',' << result.after_init.rss_bytes << ','
        << result.before_insert.rss_bytes << ',' << result.before_init.heap_bytes << ','
        << result.after_init.heap_bytes << ',' << result.before_insert.heap_bytes << ','
        << result.after_workload.heap_bytes << '\n';
    output.close();
    if (!output) {
        throw std::runtime_error("Failed to write insertion memory CSV: " + path);
    }
    return path;
}

} // namespace benchmark_results

#endif // SHAREHASH_RESULT_WRITER_H
