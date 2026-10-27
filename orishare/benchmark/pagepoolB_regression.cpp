// Standalone PagePool B regression tests. No benchmark defaults are changed.
// c++ -std=c++14 -O1 -g -pthread benchmark/pagepoolB_regression.cpp -o /tmp/pagepoolB_regression
// Add -fsanitize=address,undefined -fno-omit-frame-pointer for memory checks.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

// Track EXACT requested allocation sizes only while an index operation runs.
// The malloc header is test-only allocator overhead, outside the index ledger.
namespace allocation_probe {
thread_local bool enabled = false;
std::atomic<std::size_t> live{0};
std::atomic<long> fail_after{0};
struct alignas(std::max_align_t) Header { std::size_t bytes; bool tracked; };
struct Scope {
    bool previous = enabled;
    Scope() { enabled = true; }
    ~Scope() { enabled = previous; }
};
}
void* operator new(std::size_t bytes) {
    using namespace allocation_probe;
    if (enabled && fail_after.load() > 0 && fail_after.fetch_sub(1) == 1) throw std::bad_alloc();
    if (bytes > std::numeric_limits<std::size_t>::max() - sizeof(Header)) throw std::bad_alloc();
    Header* h = static_cast<Header*>(std::malloc(sizeof(Header) + bytes));
    if (!h) throw std::bad_alloc();
    h->bytes = bytes; h->tracked = enabled;
    if (enabled) live.fetch_add(bytes);
    return h + 1;
}
void operator delete(void* ptr) noexcept {
    if (!ptr) return;
    auto* h = static_cast<allocation_probe::Header*>(ptr) - 1;
    if (h->tracked) allocation_probe::live.fetch_sub(h->bytes);
    std::free(h);
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void* ptr) noexcept { ::operator delete(ptr); }
void operator delete(void* ptr, std::size_t) noexcept { ::operator delete(ptr); }
void operator delete[](void* ptr, std::size_t) noexcept { ::operator delete(ptr); }

template<class F> auto tracked(F&& f) -> decltype(f()) { allocation_probe::Scope scope; return f(); }
void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }

struct CollisionKey {
    std::uint64_t id;
    bool operator==(const CollisionKey& other) const noexcept { return id == other.id; }
};
struct CountingKey {
    std::uint64_t id;
    bool operator==(const CountingKey& other) const noexcept { return id == other.id; }
};
static std::size_t hash_calls = 0;
namespace std {
template<> struct hash<CollisionKey> {
    size_t operator()(const CollisionKey&) const noexcept { return 0; }
};
template<> struct hash<CountingKey> {
    size_t operator()(const CountingKey& key) const noexcept { ++hash_calls; return key.id; }
};
}
#include "../include/ShareHash_pagepoolB.h"
using Index = ShareHash::sharehash_subtable<std::uint64_t, std::uint64_t>;

template<class T> void ledger(const T& index) {
    auto b = index.getSpaceBudget();
    require(b.current_live_bytes == sizeof(index) + allocation_probe::live.load(), "requested-byte ledger mismatch");
    require(b.current_live_bytes <= b.physical_limit_bytes, "physical budget exceeded");
    require(b.live_keys_main == index.size() && b.live_keys_fallback == 0, "live-key ledger mismatch");
}
template<class T> std::unique_ptr<T> make_index() {
    // The index OBJECT itself is not tracked; its sizeof is explicitly in ledger.
    void* raw = ::operator new(sizeof(T));
    try { return std::unique_ptr<T>(tracked([&] { return new (raw) T(); })); }
    catch (...) { ::operator delete(raw); throw; }
}
void growth_and_addresses() {
    auto index = make_index<Index>();
    ledger(*index);
    std::vector<std::pair<std::uint64_t, const Index::bucketItem*>> addresses;
    std::mt19937_64 gen(20261027);
    std::vector<std::uint64_t> keys;
    for (std::uint64_t i = 0; i < 60000; ++i) {
        auto key = gen(); keys.push_back(key);
        require(tracked([&] { return index->insert(key, key ^ 0xfedcba9876543210ULL); }), "random insert rejected");
        if (i < 100 || i % 997 == 0) addresses.push_back({key, index->getRecordAddress(key)});
        if (i % 127 == 0) ledger(*index);
    }
    std::size_t splits = 0, relinked = 0;
    for (std::size_t i = 0; i < index->getSubtableCount(); ++i) {
        auto s = index->getSubtableStats(i); splits += s.index_split_count; relinked += s.relinked_handles;
        require(s.migrated_items == 0, "records migrated");
    }
    require(splits && relinked, "index never split");
    for (auto pair : addresses) require(index->getRecordAddress(pair.first) == pair.second, "record address moved");
    for (auto key : keys) { std::uint64_t v = 0; require(index->get(key, v) && v == (key ^ 0xfedcba9876543210ULL), "wrong random value"); }
    for (std::size_t i = 0; i < keys.size(); i += 3)
        require(tracked([&] { return index->remove(keys[i]); }), "remove failed");
    for (std::size_t i = 0; i < keys.size(); ++i) {
        std::uint64_t v = 0; bool found = index->get(keys[i], v);
        require(found == (i % 3 != 0), "deleted key lookup mismatch");
        if (found) require(v == (keys[i] ^ 0xfedcba9876543210ULL), "value changed after remove");
    }
    for (std::size_t i = 0; i < keys.size(); i += 3)
        require(tracked([&] { return index->insert(keys[i], i); }), "vacated slot not reused");
    ledger(*index);
    tracked([&] { index->initShareHash(); }); ledger(*index);
    require(index->size() == 0, "reset retained records");
    tracked([&] { index->deleteShareHash(); }); ledger(*index);
    require(index->getPageCount() == 0 && !index->insert(7, 8), "released index remained writable");
    tracked([&] { index->initShareHash(); }); ledger(*index);
    require(tracked([&] { return index->insert(7, 8); }), "reinitialization failed");
}
void collision_and_duplicates() {
    using T = ShareHash::sharehash_subtable<CollisionKey, std::uint64_t>;
    typename T::SpaceBudget budget; budget.a_stable_factor = budget.a_migration_factor = 256;
    void* raw = ::operator new(sizeof(T));
    std::unique_ptr<T> index(tracked([&] { return new (raw) T(1, 0.8, 1, 0, budget); }));
    for (std::uint64_t i = 0; i < 1500; ++i)
        require(tracked([&] { return index->insert({i}, i + 77); }), "collision insert failed");
    for (std::uint64_t i = 0; i < 1500; ++i) {
        std::uint64_t v = 0; require(index->get({i}, v) && v == i + 77, "collision lookup wrong");
    }
    std::uint64_t v = 0; require(!index->get({999999}, v), "collision miss became hit");
    for (std::uint64_t i = 0; i < 1500; ++i)
        require(tracked([&] { return index->remove({i}); }), "collision remove failed");
    ledger(*index);
    require(!tracked([&] { return index->insert({42}, 0); }), "retained pages bypassed post-delete budget");
    tracked([&] { index->deleteShareHash(); index->initShareHash(); });
    std::vector<bool> seen(100, false);
    for (std::uint64_t i = 0; i < 100; ++i)
        require(tracked([&] { return index->insert({42}, i); }), "duplicate insert failed");
    for (std::uint64_t i = 0; i < 100; ++i) {
        require(index->get({42}, v) && v < 100 && !seen[v], "duplicate value lost/repeated"); seen[v] = true;
        require(tracked([&] { return index->remove({42}); }), "duplicate remove failed");
    }
    require(!index->get({42}, v) && index->size() == 0, "duplicate count wrong"); ledger(*index);
}
void no_rehash_on_split() {
    using T = ShareHash::sharehash_subtable<CountingKey, std::uint64_t>;
    typename T::SpaceBudget b; b.a_stable_factor = b.a_migration_factor = 128;
    void* raw = ::operator new(sizeof(T));
    std::unique_ptr<T> index(tracked([&] { return new (raw) T(1, 0.8, 1, 0, b); }));
    hash_calls = 0;
    for (std::uint64_t i = 0; i < 20000; ++i)
        require(tracked([&] { return index->insert({i}, i + 1); }), "counting insert failed");
    require(hash_calls == 20000, "growth rehashed stored records");
    require(index->getSubtableStats(0).index_split_count > 100, "no index splits exercised"); ledger(*index);
}
void rejection_and_reset() {
    Index::SpaceBudget b; b.a_stable_factor = b.a_migration_factor = 0;
    void* raw = ::operator new(sizeof(Index));
    std::unique_ptr<Index> index(tracked([&] { return new (raw) Index(1, 0.8, 16, 0, b); }));
    std::uint64_t accepted = 0;
    while (accepted < 1000 && tracked([&] { return index->insert(accepted, accepted + 3); })) ++accepted;
    require(accepted > 0 && accepted < 1000, "zero-slope budget did not reject"); ledger(*index);
    for (std::uint64_t i = 0; i < accepted; ++i) {
        std::uint64_t v=0; require(index->get(i,v) && v==i+3, "budget rejection damaged record");
    }
    require(!index->resize() && !index->resize(1), "manual resize bypassed budget");
    bool reset_denied = false;
    try { tracked([&] { index->initShareHash(); }); } catch (const std::length_error&) { reset_denied = true; }
    require(reset_denied && index->size() == accepted, "reset bypassed coexistence budget"); ledger(*index);
}
void allocation_failures() {
    for (long failure = 1; failure <= 14; ++failure) {
        auto before = allocation_probe::live.load();
        allocation_probe::fail_after = failure;
        bool failed = false;
        try { auto index = make_index<Index>(); }
        catch (const std::bad_alloc&) { failed = true; }
        allocation_probe::fail_after = 0;
        require(failed, "initialization failure point was not reached");
        require(allocation_probe::live.load() == before, "initialization allocation leaked");
    }
    // 16 buckets: the next insertion needs an index bucket block and KV page.
    // 2048 buckets: page 65 additionally needs a radix leaf node.
    for (std::size_t initial : {std::size_t(16), std::size_t(2048)}) {
        for (long failure = 1; failure <= 6; ++failure) {
            Index::SpaceBudget b; b.a_stable_factor = b.a_migration_factor = 256;
            void* raw = ::operator new(sizeof(Index));
            std::unique_ptr<Index> index(tracked([&] { return new (raw) Index(1, 0.8, initial, 0, b); }));
            auto n = static_cast<std::uint64_t>(0.8L * index->getItemNumber());
            for (std::uint64_t i=0;i<n;++i) require(tracked([&] {return index->insert(i,i+9);}), "failure setup rejected");
            const auto* address = index->getRecordAddress(0);
            allocation_probe::fail_after = failure;
            bool inserted = tracked([&] { return index->insert(n,n+9); });
            allocation_probe::fail_after = 0;
            require(index->size() == n + inserted, "failed insert changed record count"); ledger(*index);
            require(address == index->getRecordAddress(0), "failed growth moved record");
            for(std::uint64_t i=0;i<n;++i) { std::uint64_t v=0; require(index->get(i,v)&&v==i+9,"failure damaged record"); }
            if (!inserted) require(tracked([&] { return index->insert(n,n+9); }), "retry after failure failed");
            ledger(*index);
        }
    }
}
void failed_reset_retains_old_storage() {
    for(long failure=1;failure<=8;++failure){
        Index::SpaceBudget b; b.a_stable_factor=b.a_migration_factor=256;
        void* raw=::operator new(sizeof(Index));
        std::unique_ptr<Index> index(tracked([&]{return new(raw) Index(1,0.8,16,0,b);}));
        for(std::uint64_t i=0;i<1000;++i)require(tracked([&]{return index->insert(i,i+17);}),"reset failure setup rejected");
        auto previous=index->getSpaceBudget().current_live_bytes;
        auto address=index->getRecordAddress(0);
        allocation_probe::fail_after=failure;
        bool failed=false;
        try{tracked([&]{index->initShareHash();});}catch(const std::bad_alloc&){failed=true;}
        allocation_probe::fail_after=0;
        require(failed && index->size()==1000 && index->getSpaceBudget().current_live_bytes==previous,"failed reset changed old state");
        require(index->getRecordAddress(0)==address,"failed reset moved old records");
        for(std::uint64_t i=0;i<1000;++i){std::uint64_t v=0;require(index->get(i,v)&&v==i+17,"failed reset damaged value");}
        ledger(*index);
    }
}
void radix_boundaries() {
    using Directory=Index::PageDirectory<int>;
    const std::size_t count=262146; // Cross 64, 4096 and 262144 entry boundaries.
    std::vector<int> values(count);
    Directory directory;
    for(std::size_t i=0;i<count;++i){
        values[i]=static_cast<int>(i);
        tracked([&]{Directory::Prepared path(directory.nodes_for_append());directory.append(&values[i],path);});
    }
    require(allocation_probe::live==Directory::node_count(count)*sizeof(Directory::Node),"radix node accounting mismatch");
    for(std::size_t i=0;i<count;++i)require(directory.at(i)==&values[i]&&*directory.at(i)==static_cast<int>(i),"radix boundary lookup wrong");
}
void finite_cap_and_validation() {
    std::size_t initial_bytes = 0;
    { Index::SpaceBudget b; void* raw=::operator new(sizeof(Index));
      std::unique_ptr<Index> probe(tracked([&]{return new(raw) Index(1,0.8,16,0,b);}));
      initial_bytes=probe->getSpaceBudget().current_live_bytes; }
    {
        Index::SpaceBudget b; b.physical_limit_bytes=initial_bytes+256;
        void* raw=::operator new(sizeof(Index));
        std::unique_ptr<Index> index(tracked([&]{return new(raw) Index(1,0.8,16,0,b);}));
        std::uint64_t count=0;
        while(count<1000&&tracked([&]{return index->insert(count,count);}))++count;
        require(count>0&&count<1000,"finite cap did not reject"); ledger(*index);
    }
    for (int mode=0;mode<4;++mode) {
        bool denied=false;
        try {
            allocation_probe::Scope scope;
            Index::SpaceBudget b;
            if(mode==0) b.additive_bytes=1;
            if(mode==1) b.physical_limit_bytes=initial_bytes-1;
            if(mode==2) b.a_stable_factor=std::numeric_limits<double>::quiet_NaN();
            Index bad(mode==3 ? 3 : 1,0.8,16,0,b);
        } catch(const std::exception&) {denied=true;}
        require(denied,"invalid configuration accepted");
    }
}
void concurrent_access() {
    auto index=make_index<Index>();
    for(std::uint64_t i=0;i<1000;++i)require(tracked([&]{return index->insert(i,i+1);}),"concurrency setup failed");
    std::atomic<int> failures{0};
    std::vector<std::thread> workers;
    for(std::uint64_t worker=0;worker<4;++worker)workers.emplace_back([&,worker]{
        for(std::uint64_t i=0;i<5000;++i){
            auto key=1000+worker*5000+i;
            if(!tracked([&]{return index->insert(key,key+1);}))++failures;
            std::uint64_t v=0;
            if(!index->get(i%1000,v)||v!=i%1000+1)++failures;
        }
    });
    for(auto& worker:workers)worker.join();
    require(failures==0 && index->size()==21000,"concurrent insert/get failed"); ledger(*index);
    workers.clear();
    for(std::uint64_t worker=0;worker<4;++worker)workers.emplace_back([&,worker]{
        for(std::uint64_t i=0;i<5000;++i){
            auto key=1000+worker*5000+i;
            if(!tracked([&]{return index->remove(key);}))++failures;
        }
    });
    for(auto& worker:workers)worker.join();
    require(failures==0 && index->size()==1000,"concurrent remove failed"); ledger(*index);
}
int main(){
    try {
        const std::pair<const char*,void(*)()> tests[]={
            {"growth_values_addresses_remove_reset",growth_and_addresses},
            {"collisions_and_duplicates",collision_and_duplicates},
            {"cached_hash_splits",no_rehash_on_split},
            {"affine_rejection_and_reset_peak",rejection_and_reset},
            {"allocation_failure_rollback",allocation_failures},
            {"failed_reset_preserves_storage",failed_reset_retains_old_storage},
            {"radix_directory_boundaries",radix_boundaries},
            {"finite_cap_and_invalid_config",finite_cap_and_validation},
            {"concurrent_access",concurrent_access}};
        for(auto test:tests){
            require(allocation_probe::live==0,"tracked allocations survived previous test");
            test.second();
            require(allocation_probe::live==0,"test leaked index allocations");
            std::cout<<"PASS "<<test.first<<'\n';
        }
    }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
