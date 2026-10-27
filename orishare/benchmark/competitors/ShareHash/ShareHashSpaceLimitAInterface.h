#ifndef SHAREHASH_SPACE_LIMIT_A_INTERFACE_H
#define SHAREHASH_SPACE_LIMIT_A_INTERFACE_H

// The supplied variant intentionally retains the original template name.
// Rename only that token while including it, so two non-equivalent templates
// cannot violate ODR or silently share linker symbols in one benchmark binary.
// Neither algorithm header nor its object layout/operations are modified.
#ifdef sharehash_subtable
#error "unexpected sharehash_subtable macro before SpaceLimitA adapter"
#endif
#define sharehash_subtable sharehash_subtable_spacelimitA
#include "ShareHash_subtable_spacelimitA.h"
#undef sharehash_subtable

#include "../MapInterface.h"
#include "../../ResultWriter.h"
#include <algorithm>
#include <limits>
#include <memory>

template<class Key, class Value>
class ShareHashSpaceLimitAInterface : public MapInterface<Key, Value> {
    using Index = ShareHash::sharehash_subtable_spacelimitA<Key, Value>;
    std::unique_ptr<Index> index;

    static std::size_t allowed_bytes(const typename Index::SpaceBudget& budget, double factor) {
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        const long double scaled = static_cast<long double>(factor) * budget.live_keys_main;
        const std::size_t remaining = maximum - budget.additive_bytes;
        if (scaled >= static_cast<long double>(remaining))
            return budget.physical_limit_bytes;
        return std::min(budget.physical_limit_bytes,
                        budget.additive_bytes + static_cast<std::size_t>(scaled));
    }
    long long lock_bytes() const {
        return index ? (index->getItemNumber()
            + static_cast<long long>(index->getSubtableCount()) + 1)
            * static_cast<long long>(sizeof(ShareHash::SpinLock)) : 0;
    }
public:
    void init() override { index = std::make_unique<Index>(); }
    void bulk_load(std::pair<Key, Value>* pairs, long long count) override {
        for (long long i = 0; i < count; ++i) index->insert(pairs[i].first, pairs[i].second);
    }
    bool get(Key key, Value& value) override { return index->get(key, value); }
    bool insert(Key key, Value value) override { return index->insert(key, value); }
    long long memory_consumption() override { return index->memoryUsage(); }
    long long lock_memory_consumption() override {
        // Slot locks + T subtable locks + the global budget lock.
        return lock_bytes();
    }

    // Extra diagnostics are persisted after the existing insertion memory
    // snapshot, outside both timing intervals, including rejected workloads.
    void save_budget_csv(const std::string& directory, int requested, int threads,
                         std::uint64_t seed, long long successes, double seconds) const {
        const auto budget = index->getSpaceBudget();
        const long long locks = lock_bytes();
        auto out = benchmark_results::open_csv(directory, "space_budget_results.csv",
            "timestamp_utc,hashmap_type,dataset_size,thread_count,seed,insert_successes,"
            "insert_rejections,insert_seconds,successful_insert_throughput_ops_s,"
            "insert_acceptance_fraction,slot_capacity,subtable_count,physical_limit_bytes,"
            "a_stable_factor,a_migration_factor,additive_bytes,min_load_factor,max_load_factor,"
            "current_live_bytes,peak_live_bytes,live_keys_main,live_keys_fallback,"
            "allowed_stable_bytes,allowed_migration_bytes,post_insert_load_factor,"
            "lock_memory_bytes,reported_bpk_per_stored_key_excluding_locks");
        const double actual = static_cast<double>(budget.live_keys_main);
        out << benchmark_results::utc_timestamp() << ",sharehash_spacelimitA,"
            << requested << ',' << threads << ',' << seed << ',' << successes << ','
            << requested - successes << ',' << seconds << ',' << successes / seconds << ','
            << static_cast<double>(successes) / requested << ',' << index->getItemNumber() << ','
            << index->getSubtableCount() << ',' << budget.physical_limit_bytes << ','
            << budget.a_stable_factor << ',' << budget.a_migration_factor << ','
            << budget.additive_bytes << ',' << index->min_load_factor() << ','
            << index->max_load_factor() << ',' << budget.current_live_bytes << ','
            << budget.peak_live_bytes << ',' << budget.live_keys_main << ','
            << budget.live_keys_fallback << ','
            << allowed_bytes(budget, budget.a_stable_factor) << ','
            << allowed_bytes(budget, budget.a_migration_factor) << ','
            << actual / index->getItemNumber() << ',' << locks << ','
            << (actual ? (static_cast<double>(budget.current_live_bytes) - locks) / actual : 0.0)
            << '\n';
        out.close();
        if (!out) throw std::runtime_error("Failed to save space-budget diagnostics");
    }
};

#endif // SHAREHASH_SPACE_LIMIT_A_INTERFACE_H
