// PagePool B, derived from V2-A's API, lock ordering and strict byte admission.
// Record pages never move. Linear hashing splits only cached-hash/slot handles;
// growing storage alone cannot keep queries short when logical buckets are fixed.
// Expected O(1) lookup under a distributed hash, NOT a zero-latency/worst-case claim.
// Requested owned bytes include pages, index blocks, radix directories and locks.
// Allocator overhead, user-owned inputs and thread stacks are outside this model.
// remove() does not shrink pages, so retained bytes can exceed a*n+b after deletes;
// subsequent writes/growth must pass admission again. No fallback storage exists.
// Like the other alternative headers, include one sharehash_subtable definition
// per translation unit, or rename that token in a benchmark adapter.
#ifndef SHAREHASH_SHAREHASH_PAGEPOOL_B_H
#define SHAREHASH_SHAREHASH_PAGEPOOL_B_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include "concurrency.h"

namespace ShareHash {

template<typename Key, typename Value>
class sharehash_subtable {
public:
    static constexpr std::size_t PAGE_SLOTS = 128;
    static constexpr std::size_t INDEX_SLOTS = 8;
    static constexpr std::size_t BUCKETS_PER_BLOCK = 16;
    using bitmap_t = std::uint8_t;

    struct bucketItem {
        Key key;
        Value value;
        bucketItem(const Key& k, const Value& v)
            noexcept(std::is_nothrow_copy_constructible<Key>::value &&
                     std::is_nothrow_copy_constructible<Value>::value)
            : key(k), value(v) {}
    };
    using item_storage_t = typename std::aligned_storage<sizeof(bucketItem),
                                                        alignof(bucketItem)>::type;
    struct SpaceBudget {
        std::size_t physical_limit_bytes = std::numeric_limits<std::size_t>::max();
        // Handles and radix metadata are real costs. Do not reuse V2-A's 32
        // bytes/key without measurement. Both admission factors can be equal.
        double a_stable_factor = 48.0;
        double a_migration_factor = 48.0;
        std::size_t current_live_bytes = 0;
        std::size_t live_keys_main = 0;
        std::size_t live_keys_fallback = 0;
        std::size_t additive_bytes = std::numeric_limits<std::size_t>::max();
        std::size_t peak_live_bytes = 0;
    };

    struct Page {
        item_storage_t items[PAGE_SLOTS];
        bitmap_t bitmap[PAGE_SLOTS / 8];
        SpinLock locks[PAGE_SLOTS];
        Page* pool_next = nullptr;
        Page* free_next = nullptr;
        std::uint32_t local_id = 0;
        std::uint16_t used = 0;
        explicit Page(std::uint32_t id) : local_id(id) {
            std::memset(bitmap, 0xff, sizeof(bitmap));
        }
        bucketItem* item(std::size_t offset) noexcept {
            return reinterpret_cast<bucketItem*>(&items[offset]);
        }
        const bucketItem* item(std::size_t offset) const noexcept {
            return reinterpret_cast<const bucketItem*>(&items[offset]);
        }
        bool empty(std::size_t offset) const noexcept {
            return (bitmap[offset / 8] & (bitmap_t(1) << (offset % 8))) != 0;
        }
        ~Page() {
            std::allocator<bucketItem> alloc;
            for (std::size_t i = 0; i < PAGE_SLOTS; ++i)
                if (!empty(i))
                    std::allocator_traits<std::allocator<bucketItem>>::destroy(alloc, item(i));
        }
        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;
    };

    // Global owner; reads use the target subtable's radix directory directly.
    // No vector capacity, reallocations or unaccounted ownership nodes.
    class PagePool {
        Page* head_ = nullptr;
        std::size_t count_ = 0;
    public:
        PagePool() = default;
        ~PagePool() { clear(); }
        PagePool(const PagePool&) = delete;
        PagePool& operator=(const PagePool&) = delete;
        void adopt(Page* page) noexcept {
            page->pool_next = head_; head_ = page; ++count_;
        }
        void clear() noexcept {
            while (head_) {
                Page* next = head_->pool_next;
                delete head_; head_ = next;
            }
            count_ = 0;
        }
        std::size_t size() const noexcept { return count_; }
    };

    // Fixed four-level radix directory: 24-bit IDs, four bounded pointer loads.
    // One append creates at most four 512-byte nodes on a 64-bit host. There is
    // no linked traversal and no whole-directory replacement/copy peak.
    template<class T> class PageDirectory {
    public:
        struct Node { void* entries[64] = {}; };
        struct Prepared {
            std::unique_ptr<Node> nodes[4];
            explicit Prepared(std::size_t count) {
                for (std::size_t i = 0; i < count; ++i) nodes[i].reset(new Node());
            }
        };
        static constexpr std::size_t limit = std::size_t(1) << 24;
    private:
        Node* root_ = nullptr;
        std::size_t size_ = 0;
        static void clear_nodes(Node* node, unsigned level) noexcept {
            if (!node) return;
            if (level)
                for (void* entry : node->entries)
                    clear_nodes(static_cast<Node*>(entry), level - 1);
            delete node;
        }
    public:
        PageDirectory() = default;
        ~PageDirectory() { clear_nodes(root_, 3); }
        PageDirectory(const PageDirectory&) = delete;
        PageDirectory& operator=(const PageDirectory&) = delete;
        std::size_t size() const noexcept { return size_; }
        static std::size_t node_count(std::size_t count) {
            if (!count) return 0;
            if (count > limit) throw std::length_error("radix directory exhausted");
            return 1 + (count + 63) / 64 + (count + 4095) / 4096
                     + (count + 262143) / 262144;
        }
        std::size_t nodes_for_append() const {
            if (size_ == limit) throw std::length_error("radix directory exhausted");
            const Node* node = root_;
            for (int level = 3; level >= 0; --level) {
                if (!node) return static_cast<std::size_t>(level + 1);
                if (level) node = static_cast<const Node*>(
                    node->entries[(size_ >> (6 * level)) & 63]);
            }
            return 0;
        }
        void append(T* value, Prepared& prepared) noexcept {
            std::size_t used = 0;
            if (!root_) root_ = prepared.nodes[used++].release();
            Node* node = root_;
            for (int level = 3; level > 0; --level) {
                void*& entry = node->entries[(size_ >> (6 * level)) & 63];
                if (!entry) entry = prepared.nodes[used++].release();
                node = static_cast<Node*>(entry);
            }
            node->entries[size_ & 63] = value; ++size_;
        }
        T* at(std::size_t index) const noexcept {
            const Node* node = root_;
            for (int level = 3; level > 0; --level)
                node = static_cast<const Node*>(node->entries[(index >> (6 * level)) & 63]);
            return static_cast<T*>(node->entries[index & 63]);
        }
    };

    // Cached 32-bit routing hash: splits never hash or move stored records.
    // Equal routing hashes still compare the complete Key; collisions are safe.
    struct Handle { std::uint32_t slot; std::uint32_t hash; };
    struct IndexBlock {
        Handle entries[INDEX_SLOTS];
        IndexBlock* next = nullptr;
        std::size_t count = 0;
    };
    struct bucketInfo {
        IndexBlock head;
        IndexBlock* tail = &head;
        std::size_t counter = 0;
        ~bucketInfo() {
            IndexBlock* block = head.next;
            while (block) { IndexBlock* next = block->next; delete block; block = next; }
        }
        bucketInfo() = default;
        bucketInfo(const bucketInfo&) = delete;
        bucketInfo& operator=(const bucketInfo&) = delete;
    };
    struct BucketBlock {
        bucketInfo buckets[BUCKETS_PER_BLOCK];
        BucketBlock* owned_next = nullptr;
    };
    struct topIndex {
        PageDirectory<Page> page_ids;
        PageDirectory<BucketBlock> bucket_directory;
        BucketBlock* owned_buckets = nullptr;
        Page* free_pages = nullptr;
        std::size_t total_slots = 0;
        std::size_t bucket_num = 0;
        std::size_t base = 0;
        std::size_t split = 0;
        std::size_t element_count = 0;
        std::size_t resize_count = 0;
        std::size_t index_split_count = 0;
        std::size_t relinked_handles = 0;
        mutable SpinLock resize_lock_;
        ~topIndex() {
            while (owned_buckets) {
                BucketBlock* next = owned_buckets->owned_next;
                delete owned_buckets; owned_buckets = next;
            }
        }
        topIndex() = default;
        topIndex(const topIndex&) = delete;
        topIndex& operator=(const topIndex&) = delete;
    };
    struct SubtableStats {
        std::size_t bucket_num, item_num, element_count, resize_count, migrated_items;
        // No contiguous array views exist in this representation.
        const void* buckets;
        const void* bucket_items;
        const void* none_bitmap;
        const void* locks_;
        std::size_t page_count, index_split_count, relinked_handles;
    };

private:
    static constexpr std::size_t bucket_size = 4; // Initial physical slots per requested bucket.
    static constexpr std::size_t default_initial_buckets = 5000;
    // 24-bit page directory IDs * 128 slots. Counts use size_t/long long;
    // a physical slot handle fits in uint32_t throughout this range.
    static constexpr std::size_t max_table_slots = std::size_t(1) << 31;
    class LockGuard {
        SpinLock& lock_;
    public:
        explicit LockGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
        ~LockGuard() { lock_.unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };
    struct Storage {
        topIndex* tables = nullptr;
        std::size_t table_count = 0;
        PagePool pool;
        std::size_t slots = 0;
        explicit Storage(std::size_t count) : table_count(count) {
            std::allocator<topIndex> alloc;
            tables = alloc.allocate(count);
            std::size_t constructed = 0;
            try {
                for (; constructed < count; ++constructed)
                    std::allocator_traits<std::allocator<topIndex>>::construct(alloc, tables + constructed);
            } catch (...) {
                while (constructed)
                    std::allocator_traits<std::allocator<topIndex>>::destroy(alloc, tables + --constructed);
                alloc.deallocate(tables, count); throw;
            }
        }
        ~Storage() {
            std::allocator<topIndex> alloc;
            for (std::size_t i = 0; i < table_count; ++i)
                std::allocator_traits<std::allocator<topIndex>>::destroy(alloc, tables + i);
            alloc.deallocate(tables, table_count);
        }
        Storage(const Storage&) = delete;
        Storage& operator=(const Storage&) = delete;
    };
    std::unique_ptr<Storage> storage_;
    std::size_t subtable_count_ = 0, subtable_bits_ = 0, initial_bucket_count_ = 0;
    std::size_t initial_slot_capacity_ = 0;
    double max_load_factor_ = 0.8;
    std::hash<Key> hash_func_;
    mutable SpinLock budget_lock_;
    SpaceBudget budget_;
    double min_load_factor_ = 0.75;

    static std::size_t checked_add(std::size_t a, std::size_t b) {
        if (a > std::numeric_limits<std::size_t>::max() - b)
            throw std::overflow_error("page-pool budget addition overflow");
        return a + b;
    }
    static std::size_t checked_multiply(std::size_t a, std::size_t b) {
        if (b && a > std::numeric_limits<std::size_t>::max() / b)
            throw std::overflow_error("page-pool budget multiplication overflow");
        return a * b;
    }
    static std::size_t ceil_div(std::size_t n, std::size_t d) { return n / d + (n % d != 0); }
    static std::size_t choose_subtable_count(std::size_t requested, std::size_t buckets) {
        if (!buckets || buckets > static_cast<std::size_t>(std::numeric_limits<int>::max()) / bucket_size)
            throw std::invalid_argument("initial bucket count is outside the supported range");
        if (requested) {
            if ((requested & (requested - 1)) || requested > buckets)
                throw std::invalid_argument("T must be a power of two and no greater than initial buckets");
            return requested;
        }
        const std::size_t limit = std::min<std::size_t>(256, std::max<std::size_t>(1, buckets / 128));
        std::size_t result = 1;
        while (result <= limit / 2) result *= 2;
        return result;
    }
    static SpaceBudget default_budget(std::size_t hard_limit) {
        SpaceBudget b; b.physical_limit_bytes = hard_limit; return b;
    }
    std::size_t allowed_bytes(double factor, std::size_t keys) const {
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        const long double scaled = static_cast<long double>(factor) * keys;
        if (scaled >= static_cast<long double>(maximum - budget_.additive_bytes))
            return budget_.physical_limit_bytes;
        return std::min(budget_.physical_limit_bytes,
            budget_.additive_bytes + static_cast<std::size_t>(scaled));
    }
    struct BudgetDenied {};
    class BudgetReservation {
        sharehash_subtable& owner_;
        std::size_t bytes_;
    public:
        BudgetReservation(sharehash_subtable& owner, std::size_t bytes)
            : owner_(owner), bytes_(bytes) {
            const std::size_t projected = checked_add(owner.budget_.current_live_bytes, bytes);
            if (projected > owner.allowed_bytes(owner.budget_.a_stable_factor, owner.budget_.live_keys_main)
                || projected > owner.allowed_bytes(owner.budget_.a_migration_factor, owner.budget_.live_keys_main))
                throw BudgetDenied();
            owner.budget_.current_live_bytes = projected;
            owner.budget_.peak_live_bytes = std::max(owner.budget_.peak_live_bytes, projected);
        }
        ~BudgetReservation() { owner_.budget_.current_live_bytes -= bytes_; }
        void commit() noexcept { bytes_ = 0; }
        BudgetReservation(const BudgetReservation&) = delete;
        BudgetReservation& operator=(const BudgetReservation&) = delete;
    };
    static std::size_t page_growth_bytes(const topIndex& table) {
        if (table.total_slots > max_table_slots - PAGE_SLOTS)
            throw std::length_error("page-pool subtable slot limit reached");
        return checked_add(sizeof(Page), checked_multiply(
            table.page_ids.nodes_for_append(), sizeof(typename PageDirectory<Page>::Node)));
    }
    static std::size_t index_growth_bytes(const topIndex& table) {
        if (table.bucket_num >= max_table_slots / INDEX_SLOTS)
            throw std::length_error("page-pool index limit reached");
        if (table.bucket_num % BUCKETS_PER_BLOCK) return 0;
        return checked_add(sizeof(BucketBlock), checked_multiply(
            table.bucket_directory.nodes_for_append(), sizeof(typename PageDirectory<BucketBlock>::Node)));
    }
    std::size_t initial_storage_bytes() const {
        std::size_t bytes = checked_add(sizeof(Storage), checked_multiply(subtable_count_, sizeof(topIndex)));
        for (std::size_t i = 0; i < subtable_count_; ++i) {
            const std::size_t buckets = initial_bucket_count_ / subtable_count_
                                    + (i < initial_bucket_count_ % subtable_count_);
            const std::size_t pages = ceil_div(buckets * bucket_size, PAGE_SLOTS);
            const std::size_t blocks = ceil_div(buckets, BUCKETS_PER_BLOCK);
            bytes = checked_add(bytes, checked_multiply(pages, sizeof(Page)));
            bytes = checked_add(bytes, checked_multiply(blocks, sizeof(BucketBlock)));
            bytes = checked_add(bytes, checked_multiply(PageDirectory<Page>::node_count(pages),
                                                        sizeof(typename PageDirectory<Page>::Node)));
            bytes = checked_add(bytes, checked_multiply(PageDirectory<BucketBlock>::node_count(blocks),
                                                        sizeof(typename PageDirectory<BucketBlock>::Node)));
        }
        return bytes;
    }
    std::uint64_t mixed_hash(const Key& key) const {
        std::uint64_t hash = static_cast<std::uint64_t>(hash_func_(key)) + UINT64_C(0x9e3779b97f4a7c15);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        return hash ^ (hash >> 31);
    }
    std::size_t subtable_id(std::uint64_t hash) const { return hash & (subtable_count_ - 1); }
    std::uint32_t local_hash(std::uint64_t hash) const {
        return static_cast<std::uint32_t>(hash >> subtable_bits_);
    }
    static std::size_t home_bucket(const topIndex& table, std::uint32_t hash) noexcept {
        std::size_t id = hash % table.base;
        if (id < table.split) id = hash % (table.base * 2);
        return id;
    }
    static bucketInfo& bucket_at(const topIndex& table, std::size_t id) noexcept {
        return table.bucket_directory.at(id / BUCKETS_PER_BLOCK)->buckets[id % BUCKETS_PER_BLOCK];
    }
    static Page& page_for_slot(const topIndex& table, std::uint32_t slot) noexcept {
        return *table.page_ids.at(slot / PAGE_SLOTS);
    }
    topIndex& checked_subtable(std::size_t id) const {
        if (id >= subtable_count_) throw std::out_of_range("subtable id is out of range");
        if (!storage_) throw std::logic_error("ShareHash storage has been released");
        return storage_->tables[id];
    }
    // Allocation helpers publish only after ALL relevant allocations succeeded.
    // The caller reserves bytes first and commits after publication.
    static void append_bucket_block(topIndex& table) {
        typename PageDirectory<BucketBlock>::Prepared path(table.bucket_directory.nodes_for_append());
        std::unique_ptr<BucketBlock> block(new BucketBlock());
        table.bucket_directory.append(block.get(), path);
        block->owned_next = table.owned_buckets;
        table.owned_buckets = block.release();
    }
    static void append_page(Storage& storage, topIndex& table) {
        typename PageDirectory<Page>::Prepared path(table.page_ids.nodes_for_append());
        std::unique_ptr<Page> page(new Page(static_cast<std::uint32_t>(table.page_ids.size())));
        table.page_ids.append(page.get(), path);
        page->free_next = table.free_pages;
        table.free_pages = page.get();
        storage.pool.adopt(page.release());
        table.total_slots += PAGE_SLOTS; storage.slots += PAGE_SLOTS;
    }
    bool grow_page_locked(topIndex& table) {
        const std::size_t extra = page_growth_bytes(table);
        const std::size_t new_slots = checked_add(storage_->slots, PAGE_SLOTS);
        // Initial capacity is explicitly covered by b. Beyond that startup slack,
        // enforce alpha_min on GLOBAL physical capacity, not each skewed subtable.
        if (static_cast<long double>(budget_.live_keys_main) <
            static_cast<long double>(min_load_factor_) * (new_slots - initial_slot_capacity_))
            return false;
        BudgetReservation reservation(*this, extra);
        append_page(*storage_, table);
        ++table.resize_count;
        reservation.commit(); return true;
    }
    static void append_handle_reusing(bucketInfo& bucket, Handle handle, IndexBlock*& recycled) noexcept {
        if (bucket.tail->count == INDEX_SLOTS) {
            // Dense old blocks supply enough space: ceil(a/K)+ceil(b/K)-2
            // <= ceil((a+b)/K)-1. No record or whole index allocation is needed.
            IndexBlock* block = recycled;
            recycled = block->next;
            block->next = nullptr; block->count = 0;
            bucket.tail->next = block; bucket.tail = block;
        }
        bucket.tail->entries[bucket.tail->count++] = handle;
        ++bucket.counter;
    }
    void split_index_locked(topIndex& table) {
        const std::size_t extra = index_growth_bytes(table);
        BudgetReservation reservation(*this, extra);
        if (extra) append_bucket_block(table);
        bucketInfo& left = bucket_at(table, table.split);
        bucketInfo& right = bucket_at(table, table.bucket_num);
        IndexBlock* pending = left.head.next;
        Handle saved[INDEX_SLOTS];
        std::size_t count = left.head.count;
        std::copy(left.head.entries, left.head.entries + count, saved);
        left.head.count = 0; left.head.next = nullptr; left.tail = &left.head; left.counter = 0;
        IndexBlock* recycled = nullptr;
        for (;;) {
            for (std::size_t i = 0; i < count; ++i) {
                bucketInfo& destination = saved[i].hash % (table.base * 2) == table.split ? left : right;
                append_handle_reusing(destination, saved[i], recycled);
                ++table.relinked_handles;
            }
            if (!pending) break;
            IndexBlock* block = pending; pending = block->next;
            count = block->count;
            std::copy(block->entries, block->entries + count, saved);
            block->next = recycled; recycled = block;
        }
        while (recycled) {
            IndexBlock* next = recycled->next; delete recycled; recycled = next;
            budget_.current_live_bytes -= sizeof(IndexBlock);
        }
        ++table.bucket_num; ++table.index_split_count;
        if (++table.split == table.base) { table.base *= 2; table.split = 0; }
        reservation.commit();
    }
    static std::size_t free_offset(const Page& page) noexcept {
        for (std::size_t byte = 0; byte < PAGE_SLOTS / 8; ++byte)
            if (page.bitmap[byte]) {
                unsigned bit = 0;
                while (!(page.bitmap[byte] & (bitmap_t(1) << bit))) ++bit;
                return byte * 8 + bit;
            }
        return PAGE_SLOTS; // Never reached for a page on free_pages.
    }
    bool insert_without_growth(topIndex& table, const Key& key, const Value& value, std::uint32_t hash) {
        bucketInfo& bucket = bucket_at(table, home_bucket(table, hash));
        // Overflow handles use spare index capacity without borrowing/moving KV
        // records. All blocks except the tail stay full (including after remove).
        if (bucket.tail->count == INDEX_SLOTS) {
            BudgetReservation reservation(*this, sizeof(IndexBlock));
            std::unique_ptr<IndexBlock> block(new IndexBlock());
            bucket.tail->next = block.get(); bucket.tail = block.release();
            reservation.commit();
        }
        Page& page = *table.free_pages;
        const std::size_t offset = free_offset(page);
        LockGuard guard(page.locks[offset]);
        std::allocator<bucketItem> alloc;
        std::allocator_traits<std::allocator<bucketItem>>::construct(alloc, page.item(offset), key, value);
        page.bitmap[offset / 8] &= bitmap_t(~(bitmap_t(1) << (offset % 8)));
        if (++page.used == PAGE_SLOTS) table.free_pages = page.free_next;
        const Handle handle{static_cast<std::uint32_t>(page.local_id * PAGE_SLOTS + offset), hash};
        bucket.tail->entries[bucket.tail->count++] = handle;
        ++bucket.counter; ++table.element_count; ++budget_.live_keys_main;
        return true;
    }
    // Caller holds the table lock. The hash fingerprint prevents fetching pages
    // for unrelated candidates; expected successful get loads just ONE KV page.
    static const bucketItem* find_record(const topIndex& table, const Key& key, std::uint32_t hash) {
        const bucketInfo& bucket = bucket_at(table, home_bucket(table, hash));
        for (const IndexBlock* block = &bucket.head; block; block = block->next)
            for (std::size_t i = 0; i < block->count; ++i) {
                const Handle handle = block->entries[i];
                if (handle.hash != hash) continue;
                Page& page = page_for_slot(table, handle.slot);
                const std::size_t offset = handle.slot % PAGE_SLOTS;
                if (page.item(offset)->key == key) return page.item(offset);
            }
        return nullptr;
    }

public:
    explicit sharehash_subtable(std::size_t T = 0, double max_load_factor = 0.8,
        std::size_t total_initial_buckets = default_initial_buckets, double min_load_factor = 0.75,
        std::size_t hard_limit_bytes = std::numeric_limits<std::size_t>::max())
        : sharehash_subtable(T, max_load_factor, total_initial_buckets, min_load_factor,
                            default_budget(hard_limit_bytes)) {}
    sharehash_subtable(std::size_t T, double max_load_factor, std::size_t total_initial_buckets,
        double min_load_factor, const SpaceBudget& configured_budget)
        : subtable_count_(choose_subtable_count(T, total_initial_buckets)),
          initial_bucket_count_(total_initial_buckets), max_load_factor_(max_load_factor),
          budget_(configured_budget), min_load_factor_(min_load_factor) {
        static_assert(alignof(Page) <= alignof(std::max_align_t),
                      "C++14 PagePool B does not support over-aligned records");
        static_assert(std::is_trivially_copyable<Key>::value && std::is_trivially_copyable<Value>::value,
                      "PagePool B byte accounting requires trivially copyable records");
        static_assert(std::is_nothrow_copy_constructible<Key>::value && std::is_nothrow_copy_constructible<Value>::value,
                      "PagePool B commits records using nonthrowing copies");
        if (!std::isfinite(max_load_factor_) || max_load_factor_ <= 0 || max_load_factor_ > 1)
            throw std::invalid_argument("max_load_factor must be finite and in (0,1]");
        if (!std::isfinite(min_load_factor_) || min_load_factor_ < 0 || min_load_factor_ > max_load_factor_)
            throw std::invalid_argument("min_load_factor must be finite and in [0,max_load_factor]");
        if (!std::isfinite(budget_.a_stable_factor) || budget_.a_stable_factor < 0 ||
            !std::isfinite(budget_.a_migration_factor) || budget_.a_migration_factor < 0)
            throw std::invalid_argument("space factors must be finite and nonnegative");
        for (std::size_t n = subtable_count_; n > 1; n >>= 1) ++subtable_bits_;
        budget_.current_live_bytes = sizeof(*this); budget_.peak_live_bytes = sizeof(*this);
        budget_.live_keys_main = budget_.live_keys_fallback = 0;
        if (budget_.additive_bytes == std::numeric_limits<std::size_t>::max())
            budget_.additive_bytes = checked_add(sizeof(*this), initial_storage_bytes());
        initShareHash();
    }
    ~sharehash_subtable() = default;
    sharehash_subtable(const sharehash_subtable&) = delete;
    sharehash_subtable& operator=(const sharehash_subtable&) = delete;
    sharehash_subtable(sharehash_subtable&&) = delete;
    sharehash_subtable& operator=(sharehash_subtable&&) = delete;

    void initShareHash() {
        LockGuard guard(budget_lock_);
        const std::size_t bytes = initial_storage_bytes();
        const std::size_t peak = checked_add(budget_.current_live_bytes, bytes);
        const std::size_t final_bytes = checked_add(sizeof(*this), bytes);
        if (peak > allowed_bytes(budget_.a_migration_factor, budget_.live_keys_main) ||
            final_bytes > allowed_bytes(budget_.a_stable_factor, 0))
            throw std::length_error("initialization or reset exceeds the page-pool budget");
        const std::size_t old_bytes = budget_.current_live_bytes - sizeof(*this);
        // Build the complete candidate before publication. Reset is the only
        // operation with old/new complete storage coexistence, explicitly charged.
        budget_.current_live_bytes = peak;
        budget_.peak_live_bytes = std::max(budget_.peak_live_bytes, peak);
        try {
            std::unique_ptr<Storage> candidate(new Storage(subtable_count_));
            for (std::size_t i = 0; i < subtable_count_; ++i) {
                topIndex& table = candidate->tables[i];
                const std::size_t buckets = initial_bucket_count_ / subtable_count_
                                         + (i < initial_bucket_count_ % subtable_count_);
                const std::size_t pages = ceil_div(buckets * bucket_size, PAGE_SLOTS);
                for (std::size_t j = 0; j < ceil_div(buckets, BUCKETS_PER_BLOCK); ++j)
                    append_bucket_block(table);
                table.base = table.bucket_num = buckets;
                for (std::size_t j = 0; j < pages; ++j) append_page(*candidate, table);
            }
            storage_.swap(candidate);
            candidate.reset();
        } catch (...) {
            budget_.current_live_bytes -= bytes;
            throw;
        }
        budget_.current_live_bytes -= old_bytes;
        budget_.live_keys_main = budget_.live_keys_fallback = 0;
        initial_slot_capacity_ = storage_->slots;
    }
    void deleteShareHash() {
        LockGuard guard(budget_lock_);
        storage_.reset(); budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = budget_.live_keys_fallback = 0;
    }
    bool get(const Key& key, Value& value) const {
        if (!storage_) return false;
        const std::uint64_t hash = mixed_hash(key);
        topIndex& table = storage_->tables[subtable_id(hash)];
        LockGuard guard(table.resize_lock_);
        const bucketItem* record = find_record(table, key, local_hash(hash));
        if (!record) return false;
        value = record->value; return true;
    }
    bool insert(const Key& key, const Value& value) {
        if (!storage_) return false;
        const std::uint64_t hash = mixed_hash(key);
        LockGuard budget_guard(budget_lock_);
        topIndex& table = storage_->tables[subtable_id(hash)];
        LockGuard table_guard(table.resize_lock_);
        if (budget_.live_keys_main == std::numeric_limits<std::size_t>::max() ||
            budget_.current_live_bytes > allowed_bytes(budget_.a_stable_factor, budget_.live_keys_main + 1))
            return false;
        try {
            while (static_cast<long double>(table.element_count) + 1 >
                   static_cast<long double>(max_load_factor_) * table.bucket_num * INDEX_SLOTS)
                split_index_locked(table);
            while (static_cast<long double>(table.element_count) + 1 >
                   static_cast<long double>(max_load_factor_) * table.total_slots)
                if (!grow_page_locked(table)) return false;
            return insert_without_growth(table, key, value, local_hash(hash));
        } catch (const BudgetDenied&) { return false; }
          catch (const std::bad_alloc&) { return false; }
          catch (const std::overflow_error&) { return false; }
          catch (const std::length_error&) { return false; }
    }
    bool remove(const Key& key) {
        if (!storage_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::uint32_t local = local_hash(hash);
        LockGuard budget_guard(budget_lock_);
        topIndex& table = storage_->tables[subtable_id(hash)];
        LockGuard table_guard(table.resize_lock_);
        bucketInfo& bucket = bucket_at(table, home_bucket(table, local));
        for (IndexBlock* block = &bucket.head; block; block = block->next)
            for (std::size_t i = 0; i < block->count; ++i) {
                Handle handle = block->entries[i];
                if (handle.hash != local) continue;
                Page& page = page_for_slot(table, handle.slot);
                const std::size_t offset = handle.slot % PAGE_SLOTS;
                if (!(page.item(offset)->key == key)) continue;
                LockGuard slot_guard(page.locks[offset]);
                std::allocator<bucketItem> alloc;
                std::allocator_traits<std::allocator<bucketItem>>::destroy(alloc, page.item(offset));
                page.bitmap[offset / 8] |= bitmap_t(1) << (offset % 8);
                if (page.used-- == PAGE_SLOTS) { page.free_next = table.free_pages; table.free_pages = &page; }
                block->entries[i] = bucket.tail->entries[--bucket.tail->count];
                if (!bucket.tail->count && bucket.tail != &bucket.head) {
                    IndexBlock* previous = &bucket.head;
                    while (previous->next != bucket.tail) previous = previous->next;
                    delete bucket.tail; previous->next = nullptr; bucket.tail = previous;
                    budget_.current_live_bytes -= sizeof(IndexBlock);
                }
                --bucket.counter; --table.element_count; --budget_.live_keys_main;
                return true;
            }
        return false;
    }
    bool try_resize_with_budget(std::size_t id) {
        if (!storage_ || id >= subtable_count_) return false;
        LockGuard budget_guard(budget_lock_);
        topIndex& table = storage_->tables[id];
        LockGuard table_guard(table.resize_lock_);
        try { return grow_page_locked(table); }
        catch (const BudgetDenied&) { return false; }
        catch (const std::bad_alloc&) { return false; }
        catch (const std::overflow_error&) { return false; }
        catch (const std::length_error&) { return false; }
    }
    bool resize(std::size_t id = 0) { return try_resize_with_budget(id); }
    SpaceBudget getSpaceBudget() const { LockGuard guard(budget_lock_); return budget_; }
    double min_load_factor() const noexcept { return min_load_factor_; }
    double max_load_factor() const noexcept { return max_load_factor_; }
    std::size_t projected_new_table_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_); topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_); return page_growth_bytes(table);
    }
    std::size_t projected_peak_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_); topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_);
        return checked_add(budget_.current_live_bytes, page_growth_bytes(table));
    }
    std::size_t getSubtableCount() const noexcept { return subtable_count_; }
    std::size_t getSubtableId(const Key& key) const { return subtable_id(mixed_hash(key)); }
    std::size_t getSubtableBucketForKey(const Key& key) const {
        const std::uint64_t hash = mixed_hash(key); topIndex& table = checked_subtable(subtable_id(hash));
        LockGuard guard(table.resize_lock_); return home_bucket(table, local_hash(hash));
    }
    SubtableStats getSubtableStats(std::size_t id) const {
        topIndex& table = checked_subtable(id); LockGuard guard(table.resize_lock_);
        return {table.bucket_num, table.total_slots, table.element_count, table.resize_count, 0,
                nullptr, nullptr, nullptr, nullptr, table.page_ids.size(),
                table.index_split_count, table.relinked_handles};
    }
    std::size_t getSubtableItemNumber(std::size_t id) const { return getSubtableStats(id).item_num; }
    std::size_t getSubtableBucketNumber(std::size_t id) const { return getSubtableStats(id).bucket_num; }
    std::size_t getSubtableHomeBucketNumber(std::size_t id) const { return getSubtableBucketNumber(id); }
    std::size_t getSubtableSize(std::size_t id) const { return getSubtableStats(id).element_count; }
    std::size_t getSubtableResizeCount(std::size_t id) const { return getSubtableStats(id).resize_count; }
    std::size_t getSubtablePageCount(std::size_t id) const { return getSubtableStats(id).page_count; }
    std::size_t getPageCount() const { LockGuard guard(budget_lock_); return storage_ ? storage_->pool.size() : 0; }
    // Read-only diagnostic address, stable across insert/split/page growth. It is
    // invalid after remove(key), reset/delete/destruction; do not dereference it
    // concurrently with those operations. Duplicate keys have unspecified order.
    const bucketItem* getRecordAddress(const Key& key) const {
        if (!storage_) return nullptr;
        const std::uint64_t hash = mixed_hash(key); topIndex& table = storage_->tables[subtable_id(hash)];
        LockGuard guard(table.resize_lock_); return find_record(table, key, local_hash(hash));
    }
    std::size_t size() const { return getSpaceBudget().live_keys_main; }
    long long getItemNumber() const {
        LockGuard guard(budget_lock_);
        const std::size_t slots = storage_ ? storage_->slots : 0;
        if (slots > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            throw std::overflow_error("slot capacity does not fit long long");
        return static_cast<long long>(slots);
    }
    long long getBucketNumber() const {
        LockGuard guard(budget_lock_); std::size_t count = 0;
        if (storage_) for (std::size_t i = 0; i < subtable_count_; ++i) count += storage_->tables[i].bucket_num;
        return static_cast<long long>(count);
    }
    long long memoryUsage() const {
        const std::size_t bytes = getSpaceBudget().current_live_bytes;
        if (bytes > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            throw std::overflow_error("memory usage does not fit long long");
        return static_cast<long long>(bytes);
    }
    std::size_t lockMemoryUsage() const {
        LockGuard guard(budget_lock_);
        return sizeof(SpinLock) * (1 + (storage_ ? subtable_count_ + storage_->slots : 0));
    }
};
// C++14 definitions for clients that bind these public constants by reference.
template<class Key, class Value>
constexpr std::size_t sharehash_subtable<Key, Value>::PAGE_SLOTS;
template<class Key, class Value>
constexpr std::size_t sharehash_subtable<Key, Value>::INDEX_SLOTS;
template<class Key, class Value>
constexpr std::size_t sharehash_subtable<Key, Value>::BUCKETS_PER_BLOCK;
template<class Key, class Value>
template<class T>
constexpr std::size_t sharehash_subtable<Key, Value>::PageDirectory<T>::limit;
} // namespace ShareHash
#endif // SHAREHASH_SHAREHASH_PAGEPOOL_B_H
