// V2-A page-pool variant: strict allocation/write admission, no fallback and no migration.
// remove() preserves the original no-shrink semantics: after enough deletions, retained pages may
// exceed the affine bound for the reduced live-key count and later writes are denied.
// All physical slots live in fixed-size pages owned by one pool shared by every subtable.
// A subtable owns only logical bucket metadata and a page directory. Growth appends one
// page, so existing records keep both their home bucket and physical address.
#ifndef SHAREHASH_SHAREHASH_PAGEPOOL_H
#define SHAREHASH_SHAREHASH_PAGEPOOL_H

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

// 与其他 ShareHash 变体保持相同类名；若同一翻译单元需要多个变体，调用方应像
// benchmark adapter 一样在 include 前用宏把 sharehash_subtable 重命名。
template<typename Key, typename Value>
class sharehash_subtable {
public:
    static constexpr std::size_t PAGE_SLOTS = 128;

    struct bucketInfo {
        // begin_index 仅保留为逻辑桶编号的兼容快照；分页实现不再把它当连续地址。
        int begin_index = 0;
        int capacity = 0;
        int counter = 0;
        std::size_t first_slot = std::numeric_limits<std::size_t>::max();
    };

    struct bucketItem {
        Key key;
        Value value;
        bucketItem(const Key& k, const Value& v) : key(k), value(v) {}
    };

    using bitmap_t = std::uint8_t;
    using item_storage_t = typename std::aligned_storage<sizeof(bucketItem),
                                                        alignof(bucketItem)>::type;

    // 每页包含 128 个稳定地址槽位。next_slots 把属于同一逻辑桶的槽串起来；
    // pool_next 仅供 PagePool 持有所有权，不参与查询路径。
    struct Page {
        item_storage_t items[PAGE_SLOTS];
        bitmap_t bitmap[PAGE_SLOTS / 8];
        SpinLock locks[PAGE_SLOTS];
        std::size_t next_slots[PAGE_SLOTS];
        std::size_t page_id = 0;
        std::size_t subtable_id = 0;
        std::size_t logical_group_begin = 0;
        std::size_t logical_group_count = 0;
        std::unique_ptr<Page> pool_next;

        Page(std::size_t id, std::size_t sid, std::size_t group_begin,
             std::size_t group_count)
            : page_id(id), subtable_id(sid), logical_group_begin(group_begin),
              logical_group_count(group_count) {
            std::memset(bitmap, 0xff, sizeof(bitmap));
            std::fill(next_slots, next_slots + PAGE_SLOTS,
                      std::numeric_limits<std::size_t>::max());
        }

        Page(const Page&) = delete;
        Page& operator=(const Page&) = delete;

        bucketItem* item(std::size_t offset) noexcept {
            return reinterpret_cast<bucketItem*>(&items[offset]);
        }
        const bucketItem* item(std::size_t offset) const noexcept {
            return reinterpret_cast<const bucketItem*>(&items[offset]);
        }

        ~Page() {
            std::allocator<bucketItem> alloc;
            for (std::size_t i = 0; i < PAGE_SLOTS; ++i) {
                if ((bitmap[i / 8] & (bitmap_t(1) << (i % 8))) == 0) {
                    std::allocator_traits<std::allocator<bucketItem>>::destroy(
                        alloc, item(i));
                }
            }
        }
    };

    // a 的单位是 bytes / 已成功存储记录。分页后没有新旧数据表共存，默认
    // migration factor 与 stable factor 相同；单页及页目录元数据均计入 current。
    struct SpaceBudget {
        std::size_t physical_limit_bytes = std::numeric_limits<std::size_t>::max();
        double a_stable_factor = 32.0;
        double a_migration_factor = 32.0;
        std::size_t current_live_bytes = 0;
        std::size_t live_keys_main = 0;
        std::size_t live_keys_fallback = 0;
        std::size_t additive_bytes = std::numeric_limits<std::size_t>::max();
        std::size_t peak_live_bytes = 0;
    };

    // 分块目录避免 std::vector 扩容时复制完整页 ID 数组，使一次增长的额外峰值
    // 始终为 sizeof(Page) 加至多一个固定目录块，即 O(PAGE_SLOTS)。
    class PageDirectory {
    public:
        static constexpr std::size_t entries_per_block = 64;

        struct Entry {
            std::size_t page_id = 0;
            Page* page = nullptr;
        };

        struct Block {
            Entry entries[entries_per_block];
            std::size_t used = 0;
            std::unique_ptr<Block> next;
        };

    private:
        std::unique_ptr<Block> head_;
        Block* tail_ = nullptr;
        std::size_t size_ = 0;

        Block* block_for(std::size_t index) const {
            if (index >= size_) throw std::out_of_range("page directory index is out of range");
            Block* block = head_.get();
            std::size_t skip = index / entries_per_block;
            while (skip--) block = block->next.get();
            return block;
        }

    public:
        PageDirectory() = default;
        PageDirectory(const PageDirectory&) = delete;
        PageDirectory& operator=(const PageDirectory&) = delete;
        PageDirectory(PageDirectory&&) = delete;
        PageDirectory& operator=(PageDirectory&&) = delete;

        ~PageDirectory() { clear(); }

        void clear() noexcept {
            while (head_) {
                std::unique_ptr<Block> current = std::move(head_);
                head_ = std::move(current->next);
                current->next.reset();
            }
            tail_ = nullptr;
            size_ = 0;
        }

        std::size_t size() const noexcept { return size_; }
        bool empty() const noexcept { return size_ == 0; }
        bool needs_block() const noexcept {
            return tail_ == nullptr || tail_->used == entries_per_block;
        }

        static std::size_t block_count_for(std::size_t entries) noexcept {
            return entries == 0 ? 0 : 1 + (entries - 1) / entries_per_block;
        }

        // 调用方提前分配 new_block，发布阶段不再分配、不会抛异常。
        void append(std::size_t page_id, Page* page,
                    std::unique_ptr<Block> new_block) noexcept {
            if (needs_block()) {
                Block* raw = new_block.get();
                if (tail_) {
                    tail_->next = std::move(new_block);
                } else {
                    head_ = std::move(new_block);
                }
                tail_ = raw;
            }
            tail_->entries[tail_->used].page_id = page_id;
            tail_->entries[tail_->used].page = page;
            ++tail_->used;
            ++size_;
        }

        std::size_t operator[](std::size_t index) const {
            Block* block = block_for(index);
            return block->entries[index % entries_per_block].page_id;
        }

        Page* page_at(std::size_t index) const {
            Block* block = block_for(index);
            return block->entries[index % entries_per_block].page;
        }
    };

    // 所有子表共享此池。物理页地址一经发布即保持稳定，直至整个索引 reset/delete。
    class PagePool {
        std::unique_ptr<Page> pages_;
        std::size_t page_count_ = 0;
        std::size_t next_page_id_ = 0;

    public:
        PagePool() = default;
        PagePool(const PagePool&) = delete;
        PagePool& operator=(const PagePool&) = delete;
        PagePool(PagePool&&) = delete;
        PagePool& operator=(PagePool&&) = delete;
        ~PagePool() { clear(); }

        std::size_t size() const noexcept { return page_count_; }
        std::size_t next_page_id() const noexcept { return next_page_id_; }

        void adopt(std::unique_ptr<Page> page) noexcept {
            page->pool_next = std::move(pages_);
            pages_ = std::move(page);
            ++page_count_;
            ++next_page_id_;
        }

        void clear() noexcept {
            while (pages_) {
                std::unique_ptr<Page> current = std::move(pages_);
                pages_ = std::move(current->pool_next);
                current->pool_next.reset();
            }
            page_count_ = 0;
            next_page_id_ = 0;
        }

        void swap(PagePool& other) noexcept {
            pages_.swap(other.pages_);
            std::swap(page_count_, other.page_count_);
            std::swap(next_page_id_, other.next_page_id_);
        }
    };

    struct topIndex {
        // bucket_num/item_num 表示物理容量；home_bucket_num 是不随增长变化的取模域。
        int item_num = 0;
        int bucket_num = 0;
        int home_bucket_num = 0;
        bucketInfo* buckets = nullptr;

        // 连续数组字段保留为源码兼容占位；分页实现始终将它们置空。
        bucketItem* bucket_items = nullptr;
        bitmap_t* none_bitmap = nullptr;
        SpinLock* locks_ = nullptr;

        std::size_t element_count = 0;
        std::size_t resize_count = 0;
        std::size_t migrated_items = 0;
        mutable SpinLock resize_lock_;

        PageDirectory page_ids;
        std::size_t total_slots = 0;

        topIndex() = default;
        topIndex(const topIndex&) = delete;
        topIndex& operator=(const topIndex&) = delete;

        ~topIndex() {
            destroy_array(buckets, static_cast<std::size_t>(home_bucket_num));
        }
    };

    // 分页后不存在可代表整张子表的连续 items/bitmap/locks 地址，因此相应指针为 null。
    struct SubtableStats {
        std::size_t bucket_num;
        std::size_t item_num;
        std::size_t element_count;
        std::size_t resize_count;
        std::size_t migrated_items;
        const void* buckets;
        const void* bucket_items;
        const void* none_bitmap;
        const void* locks_;
    };

private:
    enum : std::size_t {
        bucket_size = 4,
        page_bucket_count = PAGE_SLOTS / bucket_size,
        search_range = 500,
        default_initial_buckets = 5000,
        auto_min_buckets_per_subtable = 128,
        auto_max_subtables = 256
    };

    static constexpr std::size_t invalid_slot = std::numeric_limits<std::size_t>::max();

    class LockGuard {
        SpinLock& lock_;
    public:
        explicit LockGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
        ~LockGuard() { lock_.unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };

    template<class T>
    static T* allocate_array(std::size_t count) {
        std::allocator<T> alloc;
        T* result = alloc.allocate(count);
        std::size_t constructed = 0;
        try {
            for (; constructed < count; ++constructed) {
                std::allocator_traits<std::allocator<T>>::construct(
                    alloc, result + constructed);
            }
        } catch (...) {
            while (constructed) {
                std::allocator_traits<std::allocator<T>>::destroy(
                    alloc, result + --constructed);
            }
            alloc.deallocate(result, count);
            throw;
        }
        return result;
    }

    template<class T>
    static void destroy_array(T* array, std::size_t count) {
        if (!array) return;
        std::allocator<T> alloc;
        for (std::size_t i = 0; i < count; ++i) {
            std::allocator_traits<std::allocator<T>>::destroy(alloc, array + i);
        }
        alloc.deallocate(array, count);
    }

    struct DirectoryDeleter {
        std::size_t count;
        explicit DirectoryDeleter(std::size_t n = 0) : count(n) {}
        void operator()(topIndex* directory) const {
            destroy_array(directory, count);
        }
    };

    using Directory = std::unique_ptr<topIndex[], DirectoryDeleter>;

    Directory top_indexes_;
    PagePool page_pool_;
    std::size_t subtable_count_ = 0;
    std::size_t subtable_bits_ = 0;
    std::size_t initial_bucket_count_ = 0;
    double max_load_factor_ = 0.8;
    std::hash<Key> hash_func_;

    // 页池分配与预算共用此锁，锁顺序固定为 budget -> subtable -> slot。
    mutable SpinLock budget_lock_;
    SpaceBudget budget_;
    double min_load_factor_ = 0.75;
    std::size_t total_slot_capacity_ = 0;

    static std::size_t checked_add(std::size_t a, std::size_t b) {
        if (a > std::numeric_limits<std::size_t>::max() - b) {
            throw std::overflow_error("space budget addition overflow");
        }
        return a + b;
    }

    static std::size_t checked_multiply(std::size_t a, std::size_t b) {
        if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
            throw std::overflow_error("space budget multiplication overflow");
        }
        return a * b;
    }

    static SpaceBudget default_budget(std::size_t hard_limit_bytes) {
        SpaceBudget result;
        result.physical_limit_bytes = hard_limit_bytes;
        return result;
    }

    std::size_t allowed_bytes(double factor, std::size_t main_keys) const {
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        const long double scaled = static_cast<long double>(factor) * main_keys;
        const std::size_t remaining = maximum - budget_.additive_bytes;
        if (scaled >= static_cast<long double>(remaining)) {
            return std::min(budget_.physical_limit_bytes, maximum);
        }
        return std::min(budget_.physical_limit_bytes,
                        budget_.additive_bytes + static_cast<std::size_t>(scaled));
    }

    static std::size_t page_count_for_buckets(std::size_t buckets) noexcept {
        return 1 + (buckets - 1) / page_bucket_count;
    }

    static std::size_t table_initial_storage_bytes(std::size_t home_buckets) {
        const std::size_t pages = page_count_for_buckets(home_buckets);
        const std::size_t rounded_slots = checked_multiply(pages, PAGE_SLOTS);
        if (rounded_slots > static_cast<std::size_t>(
                                std::numeric_limits<int>::max())) {
            throw std::length_error(
                "page-rounded subtable capacity exceeds the integer slot limit");
        }
        std::size_t bytes = sizeof(topIndex);
        bytes = checked_add(bytes,
                            checked_multiply(home_buckets, sizeof(bucketInfo)));
        bytes = checked_add(bytes, checked_multiply(pages, sizeof(Page)));
        const std::size_t blocks = PageDirectory::block_count_for(pages);
        return checked_add(bytes,
                           checked_multiply(blocks,
                                            sizeof(typename PageDirectory::Block)));
    }

    std::size_t initial_directory_bytes() const {
        std::size_t bytes = 0;
        const std::size_t quotient = initial_bucket_count_ / subtable_count_;
        const std::size_t remainder = initial_bucket_count_ % subtable_count_;
        for (std::size_t i = 0; i < subtable_count_; ++i) {
            bytes = checked_add(
                bytes, table_initial_storage_bytes(
                           quotient + (i < remainder ? 1 : 0)));
        }
        return bytes;
    }

    struct GrowthProjection {
        std::size_t allocation_bytes;
        std::size_t peak_bytes;
        std::size_t stable_bytes;
        bool needs_directory_block;
    };

    GrowthProjection growth_projection(const topIndex& table) const {
        const bool needs_block = table.page_ids.needs_block();
        std::size_t allocation = sizeof(Page);
        if (needs_block) {
            allocation = checked_add(
                allocation, sizeof(typename PageDirectory::Block));
        }
        const std::size_t final_bytes = checked_add(
            budget_.current_live_bytes, allocation);
        return {allocation, final_bytes, final_bytes, needs_block};
    }

    // 预留发生在任何 Page/Block 分配之前；分页增长没有旧存储需要在提交后扣除。
    class BudgetReservation {
        sharehash_subtable& owner_;
        std::size_t release_bytes_;
    public:
        BudgetReservation(sharehash_subtable& owner, std::size_t bytes)
            : owner_(owner), release_bytes_(bytes) {
            const std::size_t reserved = checked_add(
                owner_.budget_.current_live_bytes, bytes);
            owner_.budget_.current_live_bytes = reserved;
            owner_.budget_.peak_live_bytes = std::max(
                owner_.budget_.peak_live_bytes, reserved);
        }

        ~BudgetReservation() {
            owner_.budget_.current_live_bytes -= release_bytes_;
        }

        void commit_without_release() noexcept { release_bytes_ = 0; }

        BudgetReservation(const BudgetReservation&) = delete;
        BudgetReservation& operator=(const BudgetReservation&) = delete;
    };

    static std::size_t choose_subtable_count(std::size_t requested,
                                              std::size_t buckets) {
        if (buckets == 0 ||
            buckets > static_cast<std::size_t>(std::numeric_limits<int>::max()) /
                          bucket_size) {
            throw std::invalid_argument(
                "initial bucket count is outside the supported range");
        }
        if (requested != 0) {
            if ((requested & (requested - 1)) != 0 || requested > buckets) {
                throw std::invalid_argument(
                    "T must be a power of two and no greater than initial buckets");
            }
            return requested;
        }
        const std::size_t limit = std::min<std::size_t>(
            auto_max_subtables,
            std::max<std::size_t>(1,
                                  buckets / auto_min_buckets_per_subtable));
        std::size_t result = 1;
        while (result <= limit / 2) result *= 2;
        return result;
    }

    std::uint64_t mixed_hash(const Key& key) const {
        std::uint64_t hash = static_cast<std::uint64_t>(hash_func_(key)) +
                             UINT64_C(0x9e3779b97f4a7c15);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        return hash ^ (hash >> 31);
    }

    std::size_t subtable_id(std::uint64_t hash) const {
        return hash & (subtable_count_ - 1);
    }

    std::uint64_t local_hash(std::uint64_t hash) const {
        return hash >> subtable_bits_;
    }

    static int home_bucket(const topIndex& table, std::uint64_t hash) {
        return static_cast<int>(
            hash % static_cast<std::uint64_t>(table.home_bucket_num));
    }

    static bool page_slot_empty(const Page& page, std::size_t offset) noexcept {
        return (page.bitmap[offset / 8] &
                (bitmap_t(1) << (offset % 8))) != 0;
    }

    static void mark_page_slot_empty(Page& page, std::size_t offset) noexcept {
        page.bitmap[offset / 8] |= bitmap_t(1) << (offset % 8);
    }

    static void mark_page_slot_used(Page& page, std::size_t offset) noexcept {
        page.bitmap[offset / 8] &=
            bitmap_t(~(bitmap_t(1) << (offset % 8)));
    }

    Page& page_for_slot(const topIndex& table, std::size_t logical_slot) const {
        if (logical_slot >= table.total_slots) {
            throw std::logic_error("logical slot is outside the page directory");
        }
        Page* page = table.page_ids.page_at(logical_slot / PAGE_SLOTS);
        if (!page) throw std::logic_error("page directory contains a null page");
        return *page;
    }

    static std::size_t page_offset(std::size_t logical_slot) noexcept {
        return logical_slot % PAGE_SLOTS;
    }

    std::size_t next_owned_slot(const topIndex& table,
                                std::size_t logical_slot) const {
        Page& page = page_for_slot(table, logical_slot);
        return page.next_slots[page_offset(logical_slot)];
    }

    void set_next_owned_slot(const topIndex& table, std::size_t logical_slot,
                             std::size_t next) {
        Page& page = page_for_slot(table, logical_slot);
        page.next_slots[page_offset(logical_slot)] = next;
    }

    bool logical_slot_empty(const topIndex& table,
                            std::size_t logical_slot) const {
        Page& page = page_for_slot(table, logical_slot);
        return page_slot_empty(page, page_offset(logical_slot));
    }

    void construct_slot(const topIndex& table, std::size_t logical_slot,
                        const Key& key, const Value& value) {
        Page& page = page_for_slot(table, logical_slot);
        const std::size_t offset = page_offset(logical_slot);
        std::allocator<bucketItem> alloc;
        std::allocator_traits<std::allocator<bucketItem>>::construct(
            alloc, page.item(offset), key, value);
        mark_page_slot_used(page, offset);
    }

    void destroy_slot(const topIndex& table, std::size_t logical_slot) {
        Page& page = page_for_slot(table, logical_slot);
        const std::size_t offset = page_offset(logical_slot);
        std::allocator<bucketItem> alloc;
        std::allocator_traits<std::allocator<bucketItem>>::destroy(
            alloc, page.item(offset));
        mark_page_slot_empty(page, offset);
    }

    static std::size_t group_begin_for_bucket(std::size_t bucket) noexcept {
        return (bucket / page_bucket_count) * page_bucket_count;
    }

    static std::size_t group_count_for(const topIndex& table,
                                       std::size_t group_begin) noexcept {
        const std::size_t home_count =
            static_cast<std::size_t>(table.home_bucket_num);
        return std::min<std::size_t>(page_bucket_count,
                                     home_count - group_begin);
    }

    static void allocate_bucket_metadata(topIndex& table,
                                         std::size_t home_buckets) {
        if (home_buckets == 0 ||
            home_buckets > static_cast<std::size_t>(
                               std::numeric_limits<int>::max())) {
            throw std::length_error(
                "logical bucket count exceeds the integer limit");
        }
        table.home_bucket_num = static_cast<int>(home_buckets);
        table.buckets = allocate_array<bucketInfo>(home_buckets);
        for (std::size_t i = 0; i < home_buckets; ++i) {
            table.buckets[i].begin_index = static_cast<int>(i * bucket_size);
        }
    }

    // 把一页平均挂到最多 32 个相邻逻辑桶；同桶槽位在同一页中连续创建。
    // logical_base 等于页在该子表目录中的序号乘 PAGE_SLOTS。
    static void attach_page_slots(topIndex& table, Page& page,
                                  std::size_t logical_base) noexcept {
        const std::size_t group_count = page.logical_group_count;
        const std::size_t per_bucket = PAGE_SLOTS / group_count;
        const std::size_t extra = PAGE_SLOTS % group_count;
        std::size_t offset = 0;
        for (std::size_t relative = 0; relative < group_count; ++relative) {
            bucketInfo& bucket =
                table.buckets[page.logical_group_begin + relative];
            const std::size_t count = per_bucket + (relative < extra ? 1 : 0);
            for (std::size_t i = 0; i < count; ++i, ++offset) {
                const std::size_t logical_slot = logical_base + offset;
                page.next_slots[offset] = bucket.first_slot;
                bucket.first_slot = logical_slot;
                ++bucket.capacity;
            }
        }
        table.total_slots += PAGE_SLOTS;
        table.item_num = static_cast<int>(table.total_slots);
        table.bucket_num = static_cast<int>(table.total_slots / bucket_size);
    }

    static std::unique_ptr<typename PageDirectory::Block>
    make_directory_block_if_needed(const PageDirectory& directory) {
        if (!directory.needs_block()) {
            return std::unique_ptr<typename PageDirectory::Block>();
        }
        return std::unique_ptr<typename PageDirectory::Block>(
            new typename PageDirectory::Block());
    }

    static std::unique_ptr<Page> make_page(std::size_t page_id,
                                           std::size_t subtable,
                                           std::size_t group_begin,
                                           std::size_t group_count) {
        return std::unique_ptr<Page>(
            new Page(page_id, subtable, group_begin, group_count));
    }

    std::size_t build_initial_storage(Directory& directory,
                                      PagePool& pool) {
        const std::size_t quotient = initial_bucket_count_ / subtable_count_;
        const std::size_t remainder = initial_bucket_count_ % subtable_count_;
        std::size_t total_slots = 0;

        for (std::size_t id = 0; id < subtable_count_; ++id) {
            topIndex& table = directory[id];
            const std::size_t home_buckets =
                quotient + (id < remainder ? 1 : 0);
            allocate_bucket_metadata(table, home_buckets);

            for (std::size_t group_begin = 0; group_begin < home_buckets;
                 group_begin += page_bucket_count) {
                const std::size_t group_count =
                    std::min<std::size_t>(page_bucket_count,
                                          home_buckets - group_begin);
                if (pool.next_page_id() ==
                    std::numeric_limits<std::size_t>::max()) {
                    throw std::length_error("page id space is exhausted");
                }
                const std::size_t page_id = pool.next_page_id();
                std::unique_ptr<Page> page =
                    make_page(page_id, id, group_begin, group_count);
                std::unique_ptr<typename PageDirectory::Block> block =
                    make_directory_block_if_needed(table.page_ids);
                Page* raw_page = page.get();
                pool.adopt(std::move(page));
                table.page_ids.append(page_id, raw_page, std::move(block));
                attach_page_slots(table, *raw_page,
                                  table.total_slots);
            }
            total_slots = checked_add(total_slots, table.total_slots);
        }
        return total_slots;
    }

    std::size_t find_free_slot(const topIndex& table,
                               int bucket_id) const {
        const bucketInfo& bucket = table.buckets[bucket_id];
        if (bucket.counter >= bucket.capacity) return invalid_slot;
        for (std::size_t slot = bucket.first_slot; slot != invalid_slot;
             slot = next_owned_slot(table, slot)) {
            if (logical_slot_empty(table, slot)) return slot;
        }
        return invalid_slot;
    }

    // 从 donor 的所有权链摘下一个空槽。元素不移动，只有一个逻辑槽 ID 改变归属。
    std::size_t detach_free_slot(topIndex& table, int bucket_id) {
        bucketInfo& bucket = table.buckets[bucket_id];
        if (bucket.counter >= bucket.capacity) return invalid_slot;

        std::size_t previous = invalid_slot;
        for (std::size_t slot = bucket.first_slot; slot != invalid_slot;) {
            const std::size_t next = next_owned_slot(table, slot);
            if (logical_slot_empty(table, slot)) {
                if (previous == invalid_slot) {
                    bucket.first_slot = next;
                } else {
                    set_next_owned_slot(table, previous, next);
                }
                --bucket.capacity;
                set_next_owned_slot(table, slot, invalid_slot);
                return slot;
            }
            previous = slot;
            slot = next;
        }
        return invalid_slot;
    }

    void prepend_owned_slot(topIndex& table, int bucket_id,
                            std::size_t slot) {
        bucketInfo& bucket = table.buckets[bucket_id];
        set_next_owned_slot(table, slot, bucket.first_slot);
        bucket.first_slot = slot;
        ++bucket.capacity;
    }

    bool insert_without_growth(topIndex& table, const Key& key,
                               const Value& value, std::uint64_t hash) {
        const int home = home_bucket(table, hash);
        std::size_t slot = find_free_slot(table, home);
        if (slot == invalid_slot) {
            int donor = home + 1;
            for (; donor < table.home_bucket_num &&
                   donor - home < static_cast<int>(search_range);
                 ++donor) {
                slot = detach_free_slot(table, donor);
                if (slot != invalid_slot) break;
            }
            if (slot == invalid_slot) return false;
            prepend_owned_slot(table, home, slot);
        }

        Page& page = page_for_slot(table, slot);
        LockGuard slot_guard(page.locks[page_offset(slot)]);
        construct_slot(table, slot, key, value);
        ++table.buckets[home].counter;
        ++table.element_count;
        return true;
    }

    // 真正的增长提交：只创建一页和必要的一个目录块，不读取或迁移任何旧元素。
    bool grow_page_locked(std::size_t id, topIndex& table,
                          std::size_t target_home,
                          const GrowthProjection& projection) {
        BudgetReservation reservation(*this, projection.allocation_bytes);
        try {
            const std::size_t group_begin =
                group_begin_for_bucket(target_home);
            const std::size_t group_count =
                group_count_for(table, group_begin);
            if (page_pool_.next_page_id() ==
                std::numeric_limits<std::size_t>::max()) {
                return false;
            }

            const std::size_t page_id = page_pool_.next_page_id();
            std::unique_ptr<Page> page =
                make_page(page_id, id, group_begin, group_count);
            std::unique_ptr<typename PageDirectory::Block> block;
            if (projection.needs_directory_block) {
                block.reset(new typename PageDirectory::Block());
            }

            Page* raw_page = page.get();
            const std::size_t logical_base = table.total_slots;
            page_pool_.adopt(std::move(page));
            table.page_ids.append(page_id, raw_page, std::move(block));
            attach_page_slots(table, *raw_page, logical_base);
            total_slot_capacity_ += PAGE_SLOTS;
            ++table.resize_count;
            // migrated_items 故意保持为 0：页增长不触碰任何旧记录。
            reservation.commit_without_release();
            return true;
        } catch (const std::bad_alloc&) {
            return false;
        }
    }

    // 调用方已持有 budget_lock_ 和目标子表锁。
    bool try_grow_page_with_budget_locked(std::size_t id, topIndex& table,
                                          std::size_t target_home) {
        if (target_home >=
            static_cast<std::size_t>(table.home_bucket_num)) {
            return false;
        }
        if (table.total_slots >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) -
                PAGE_SLOTS) {
            return false;
        }
        if (total_slot_capacity_ >
            std::numeric_limits<std::size_t>::max() - PAGE_SLOTS) {
            return false;
        }

        try {
            const GrowthProjection projection = growth_projection(table);
            if (projection.peak_bytes >
                allowed_bytes(budget_.a_migration_factor,
                              budget_.live_keys_main)) {
                return false;
            }
            if (projection.stable_bytes >
                allowed_bytes(budget_.a_stable_factor,
                              budget_.live_keys_main)) {
                return false;
            }

            // 不再使用“扩容后全表至少 0.75”作为准入条件。该条件适用于整表翻倍，
            // 在共享页方案中会把局部需求重新变成无关的全局耦合；严格 a*n+b 已完整
            // 约束新增页。min_load_factor 字段仅为构造/API 兼容保留。
            return grow_page_locked(id, table, target_home, projection);
        } catch (const std::overflow_error&) {
            return false;
        }
    }

    std::size_t fullest_group_home(const topIndex& table) const noexcept {
        std::size_t best_group = 0;
        long double best_ratio = -1.0L;
        const std::size_t home_count =
            static_cast<std::size_t>(table.home_bucket_num);
        for (std::size_t begin = 0; begin < home_count;
             begin += page_bucket_count) {
            const std::size_t count =
                std::min<std::size_t>(page_bucket_count,
                                      home_count - begin);
            std::size_t used = 0;
            std::size_t capacity = 0;
            for (std::size_t i = 0; i < count; ++i) {
                used += static_cast<std::size_t>(
                    table.buckets[begin + i].counter);
                capacity += static_cast<std::size_t>(
                    table.buckets[begin + i].capacity);
            }
            const long double ratio = capacity == 0
                ? 1.0L
                : static_cast<long double>(used) / capacity;
            if (ratio > best_ratio) {
                best_ratio = ratio;
                best_group = begin;
            }
        }
        return best_group;
    }

    topIndex& checked_subtable(std::size_t id) const {
        if (id >= subtable_count_) {
            throw std::out_of_range("subtable id is out of range");
        }
        if (!top_indexes_) {
            throw std::logic_error("ShareHash storage has been released");
        }
        return top_indexes_[id];
    }

public:
    explicit sharehash_subtable(
        std::size_t T = 0, double max_load_factor = 0.8,
        std::size_t total_initial_buckets = default_initial_buckets,
        double min_load_factor = 0.75,
        std::size_t hard_limit_bytes =
            std::numeric_limits<std::size_t>::max())
        : sharehash_subtable(T, max_load_factor, total_initial_buckets,
                            min_load_factor,
                            default_budget(hard_limit_bytes)) {}

    sharehash_subtable(std::size_t T, double max_load_factor,
                       std::size_t total_initial_buckets,
                       double min_load_factor,
                       const SpaceBudget& configured_budget)
        : subtable_count_(
              choose_subtable_count(T, total_initial_buckets)),
          initial_bucket_count_(total_initial_buckets),
          max_load_factor_(max_load_factor), budget_(configured_budget),
          min_load_factor_(min_load_factor) {
        static_assert(std::is_trivially_copyable<Key>::value &&
                          std::is_trivially_copyable<Value>::value,
                      "V2-A byte accounting requires trivially copyable keys and values");
        if (!std::isfinite(max_load_factor_) || max_load_factor_ <= 0 ||
            max_load_factor_ > 1) {
            throw std::invalid_argument(
                "max_load_factor must be finite and in (0,1]");
        }
        if (!std::isfinite(min_load_factor_) || min_load_factor_ < 0 ||
            min_load_factor_ > max_load_factor_) {
            throw std::invalid_argument(
                "min_load_factor must be finite and in [0,max_load_factor]");
        }
        if (!std::isfinite(budget_.a_stable_factor) ||
            budget_.a_stable_factor < 0 ||
            !std::isfinite(budget_.a_migration_factor) ||
            budget_.a_migration_factor < 0) {
            throw std::invalid_argument(
                "space factors must be finite and nonnegative");
        }
        for (std::size_t count = subtable_count_; count > 1;
             count >>= 1) {
            ++subtable_bits_;
        }

        budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = 0;
        budget_.live_keys_fallback = 0;
        budget_.peak_live_bytes = sizeof(*this);
        if (budget_.additive_bytes ==
            std::numeric_limits<std::size_t>::max()) {
            budget_.additive_bytes = checked_add(
                sizeof(*this), initial_directory_bytes());
        }
        initShareHash();
    }

    ~sharehash_subtable() = default;
    sharehash_subtable(const sharehash_subtable&) = delete;
    sharehash_subtable& operator=(const sharehash_subtable&) = delete;
    sharehash_subtable(sharehash_subtable&&) = delete;
    sharehash_subtable& operator=(sharehash_subtable&&) = delete;

    // reset 先释放旧页，再按同一 b 重建。这样默认 a_migration == a_stable 时
    // 不会为了一个逻辑清空操作制造两套完整初始页；生命周期操作仍不可并发调用。
    void initShareHash() {
        LockGuard budget_guard(budget_lock_);

        page_pool_.clear();
        top_indexes_.reset();
        total_slot_capacity_ = 0;
        budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = 0;
        budget_.live_keys_fallback = 0;

        const std::size_t storage_bytes = initial_directory_bytes();
        const std::size_t final_bytes = checked_add(
            sizeof(*this), storage_bytes);
        if (final_bytes > allowed_bytes(budget_.a_migration_factor, 0) ||
            final_bytes > allowed_bytes(budget_.a_stable_factor, 0)) {
            throw std::length_error(
                "initialization exceeds the page-pool space budget");
        }

        BudgetReservation reservation(*this, storage_bytes);
        Directory replacement(
            allocate_array<topIndex>(subtable_count_),
            DirectoryDeleter(subtable_count_));
        PagePool replacement_pool;
        const std::size_t replacement_slots =
            build_initial_storage(replacement, replacement_pool);

        top_indexes_.swap(replacement);
        page_pool_.swap(replacement_pool);
        total_slot_capacity_ = replacement_slots;
        reservation.commit_without_release();
    }

    void deleteShareHash() {
        LockGuard budget_guard(budget_lock_);
        page_pool_.clear();
        top_indexes_.reset();
        total_slot_capacity_ = 0;
        budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = 0;
        budget_.live_keys_fallback = 0;
    }

    bool get(const Key& key, Value& value) const {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::size_t id = subtable_id(hash);

        // Page 地址在正常增长期间保持稳定；目标子表锁足以保护页目录和槽链。
        // reset/delete 与其他操作并发仍不受支持，因此查询无需串行化在预算锁上。
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        const bucketInfo& bucket =
            table.buckets[home_bucket(table, local_hash(hash))];

        for (std::size_t slot = bucket.first_slot; slot != invalid_slot;
             slot = next_owned_slot(table, slot)) {
            Page& page = page_for_slot(table, slot);
            const std::size_t offset = page_offset(slot);
            if (!page_slot_empty(page, offset)) {
                LockGuard slot_guard(page.locks[offset]);
                if (page.item(offset)->key == key) {
                    value = page.item(offset)->value;
                    return true;
                }
            }
        }
        return false;
    }

    bool insert(const Key& key, const Value& value) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::size_t id = subtable_id(hash);
        LockGuard budget_guard(budget_lock_);
        if (!top_indexes_) return false;
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);

        if (budget_.live_keys_main ==
                std::numeric_limits<std::size_t>::max() ||
            budget_.current_live_bytes >
                allowed_bytes(budget_.a_stable_factor,
                              budget_.live_keys_main + 1)) {
            return false;
        }

        const std::size_t home = static_cast<std::size_t>(
            home_bucket(table, local_hash(hash)));

        while (static_cast<long double>(table.element_count) + 1 >
               static_cast<long double>(max_load_factor_) *
                   table.total_slots) {
            if (!try_grow_page_with_budget_locked(id, table, home)) {
                return false;
            }
        }

        constexpr unsigned max_growth_attempts = 4;
        for (unsigned growths = 0;; ++growths) {
            if (insert_without_growth(table, key, value,
                                      local_hash(hash))) {
                ++budget_.live_keys_main;
                return true;
            }
            if (growths == max_growth_attempts) return false;
            if (!try_grow_page_with_budget_locked(id, table, home)) {
                return false;
            }
        }
    }

    bool remove(const Key& key) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::size_t id = subtable_id(hash);
        LockGuard budget_guard(budget_lock_);
        if (!top_indexes_) return false;
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        bucketInfo& bucket =
            table.buckets[home_bucket(table, local_hash(hash))];

        for (std::size_t slot = bucket.first_slot; slot != invalid_slot;
             slot = next_owned_slot(table, slot)) {
            Page& page = page_for_slot(table, slot);
            const std::size_t offset = page_offset(slot);
            if (!page_slot_empty(page, offset)) {
                LockGuard slot_guard(page.locks[offset]);
                if (page.item(offset)->key == key) {
                    destroy_slot(table, slot);
                    --bucket.counter;
                    --table.element_count;
                    --budget_.live_keys_main;
                    return true;
                }
            }
        }
        return false;
    }

    // 为指定子表最拥挤的逻辑桶组追加一页。保留原公开方法名，但语义由翻倍
    // 调整为单页增长；所有自动和手动增长都经过同一预算控制器。
    bool try_resize_with_budget(std::size_t id) {
        if (!top_indexes_ || id >= subtable_count_) return false;
        LockGuard budget_guard(budget_lock_);
        if (!top_indexes_) return false;
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        return try_grow_page_with_budget_locked(
            id, table, fullest_group_home(table));
    }

    bool resize(std::size_t id = 0) {
        return try_resize_with_budget(id);
    }

    SpaceBudget getSpaceBudget() const {
        LockGuard guard(budget_lock_);
        return budget_;
    }

    double min_load_factor() const noexcept { return min_load_factor_; }
    double max_load_factor() const noexcept { return max_load_factor_; }

    // 现在返回下一次单页增长需要预留的 Page + 可选目录块字节数。
    std::size_t projected_new_table_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_);
        topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_);
        return growth_projection(table).allocation_bytes;
    }

    std::size_t projected_peak_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_);
        topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_);
        return growth_projection(table).peak_bytes;
    }

    std::size_t getSubtableCount() const noexcept {
        return subtable_count_;
    }

    std::size_t getSubtableId(const Key& key) const {
        return subtable_id(mixed_hash(key));
    }

    std::size_t getSubtableBucketForKey(const Key& key) const {
        const std::uint64_t hash = mixed_hash(key);
        topIndex& table = checked_subtable(subtable_id(hash));
        LockGuard guard(table.resize_lock_);
        return static_cast<std::size_t>(
            home_bucket(table, local_hash(hash)));
    }

    SubtableStats getSubtableStats(std::size_t id) const {
        topIndex& table = checked_subtable(id);
        LockGuard guard(table.resize_lock_);
        return {static_cast<std::size_t>(table.bucket_num),
                table.total_slots,
                table.element_count,
                table.resize_count,
                table.migrated_items,
                nullptr,
                nullptr,
                nullptr,
                nullptr};
    }

    std::size_t getSubtableItemNumber(std::size_t id) const {
        return getSubtableStats(id).item_num;
    }

    std::size_t getSubtableBucketNumber(std::size_t id) const {
        return getSubtableStats(id).bucket_num;
    }

    std::size_t getSubtableSize(std::size_t id) const {
        return getSubtableStats(id).element_count;
    }

    std::size_t getSubtableResizeCount(std::size_t id) const {
        return getSubtableStats(id).resize_count;
    }

    std::size_t getSubtablePageCount(std::size_t id) const {
        topIndex& table = checked_subtable(id);
        LockGuard guard(table.resize_lock_);
        return table.page_ids.size();
    }

    std::size_t getSubtableHomeBucketNumber(std::size_t id) const {
        topIndex& table = checked_subtable(id);
        LockGuard guard(table.resize_lock_);
        return static_cast<std::size_t>(table.home_bucket_num);
    }

    std::size_t getPageCount() const {
        LockGuard guard(budget_lock_);
        return page_pool_.size();
    }

    std::size_t size() const {
        return getSpaceBudget().live_keys_main;
    }

    long long getBucketNumber() const {
        return getItemNumber() / static_cast<long long>(bucket_size);
    }

    long long getItemNumber() const {
        LockGuard guard(budget_lock_);
        if (total_slot_capacity_ >
            static_cast<std::size_t>(
                std::numeric_limits<long long>::max())) {
            throw std::overflow_error(
                "slot capacity does not fit long long");
        }
        return static_cast<long long>(total_slot_capacity_);
    }

    long long memoryUsage() const {
        const std::size_t bytes = getSpaceBudget().current_live_bytes;
        if (bytes > static_cast<std::size_t>(
                        std::numeric_limits<long long>::max())) {
            throw std::overflow_error(
                "memory usage does not fit long long");
        }
        return static_cast<long long>(bytes);
    }
};

template<typename Key, typename Value>
constexpr std::size_t sharehash_subtable<Key, Value>::PAGE_SLOTS;

template<typename Key, typename Value>
constexpr std::size_t sharehash_subtable<Key, Value>::invalid_slot;

} // namespace ShareHash

#endif // SHAREHASH_SHAREHASH_PAGEPOOL_H
