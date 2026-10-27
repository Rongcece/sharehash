#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <vector>
#ifdef UNMODIFIED_HEADER
#include "../../../include/ShareHash_subtable.h"
#else
#include "ShareHash_subtable_diagnostic.h"
#endif

using Table=ShareHash::sharehash_subtable<std::uint64_t,std::uint64_t>;
const char* reason(int r) {
    switch(r) {
    case 1:return "threshold_resize_failed";
    case 2:return "placement_resize_failed";
    case 3:return "post_resize_placement_retry_failed";
    default:return "none";
    }
}
const char* resize_reason(int r) {
    switch(r) {
    case 1:return "capacity_integer_limit";
    case 2:return "subtable_assignment_changed";
    case 3:return "candidate_migration_could_not_borrow";
    case 4:return "bad_alloc";
    default:return "none";
    }
}
int main(int argc,char**argv) {
    const std::size_t n=argc>1?std::strtoull(argv[1],nullptr,10):1000000;
    for(std::uint64_t seed:{UINT64_C(20261026),UINT64_C(20261027)}) {
        Table table;
        std::mt19937_64 gen(seed);
        std::uniform_int_distribution<std::int64_t> dis(std::numeric_limits<std::int64_t>::min(),std::numeric_limits<std::int64_t>::max());
        std::vector<std::uint64_t> keys;keys.reserve(n);
        for(std::size_t i=0;i<n;++i) keys.push_back(static_cast<std::uint64_t>(dis(gen)));
        std::vector<unsigned char> inserted(n,0);
        std::size_t failed=0;
        for(std::size_t i=0;i<n;++i) {
            const auto key=keys[i];
            const auto sid=table.getSubtableId(key);
            const auto before=table.getSubtableStats(sid);
            const auto home_before=table.getSubtableBucketForKey(key);
            const auto bucket_before=static_cast<const Table::bucketInfo*>(before.buckets)[home_before];
            inserted[i]=table.insert(key,key);
            if(!inserted[i]) {
                ++failed;
                const auto after=table.getSubtableStats(sid);
                const auto home_after=table.getSubtableBucketForKey(key);
                const auto bucket_after=static_cast<const Table::bucketInfo*>(after.buckets)[home_after];
                std::uint64_t value=0;
                const bool exists=table.get(key,value);
                std::cout<<"FAIL seed="<<seed<<" idx_zero_based="<<i<<" key="<<key<<" sid="<<sid
                    <<" home_before="<<home_before<<" home_after="<<table.getSubtableBucketForKey(key)
                    <<" buckets_before="<<before.bucket_num<<" buckets_after="<<after.bucket_num
                    <<" capacity_before="<<before.item_num<<" capacity_after="<<after.item_num
                    <<" size_before="<<before.element_count<<" size_after="<<after.element_count
                    <<" resize_count_before="<<before.resize_count<<" resize_count_after="<<after.resize_count
                    <<" home_counter_before="<<bucket_before.counter<<" home_capacity_before="<<bucket_before.capacity
                    <<" home_counter_after="<<bucket_after.counter<<" home_capacity_after="<<bucket_after.capacity
                    <<" failed_key_found="<<exists;
#ifndef UNMODIFIED_HEADER
                std::cout<<" cause="<<reason(table.diag_insert_failure)<<" resize_cause="<<resize_reason(table.diag_resize_failure);
                if(table.diag_resize_failure==3)
                    std::cout<<" migration_slot="<<table.diag_failed_migration_slot
                        <<" migration_home="<<table.diag_failed_migration_home
                        <<" migration_home_capacity="<<table.diag_candidate_home_capacity
                        <<" migration_candidate_capacity="<<table.diag_candidate_capacity
                        <<" migration_copied="<<table.diag_candidate_copied;
#endif
                std::cout<<'\n';
            }
        }
        std::size_t hit=0,successful_missing=0,successful_wrong=0,failed_present=0;
        for(std::size_t i=0;i<n;++i) {
            std::uint64_t value=0;const bool found=table.get(keys[i],value);hit+=found;
            if(inserted[i]&&!found)++successful_missing;
            if(inserted[i]&&found&&value!=keys[i])++successful_wrong;
            if(!inserted[i]&&found)++failed_present;
        }
        std::cout<<"SUMMARY seed="<<seed<<" n="<<n<<" insert_successes="<<n-failed<<" failed="<<failed
            <<" lookup_hits="<<hit<<" successful_missing="<<successful_missing<<" successful_wrong="<<successful_wrong
            <<" failed_present="<<failed_present<<'\n';
        if(successful_missing||successful_wrong||failed_present)return 2;
    }
}
