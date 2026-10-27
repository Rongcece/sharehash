// V2-A: strict storage-byte budget, no fallback. All changes stay in this header.
// Alternative to ShareHash_subtable.h: retains the same class name; include one variant.
// Budget counts requested index-owned bytes (including locks/metadata), not process RSS,
// allocator rounding/headers, stacks outside the explicitly counted candidate, or user data.
// The affine bound is enforced on initialization, successful inserts and growth. remove()
// preserves its semantics and does not shrink; after deletions, later writes may be denied.
#ifndef SHAREHASH_SHAREHASH_SUBTABLE_SPACELIMIT_A_H
#define SHAREHASH_SHAREHASH_SUBTABLE_SPACELIMIT_A_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <type_traits>
#include "concurrency.h"

namespace ShareHash {

// [子表化修改1] 独立类型/保护符，可与原 ShareHash.h 同时包含。
// 参考 DySECT 的独立子表存储与单表增长思想：
// https://github.com/TooBiased/DySECT/blob/master/include/cuckoo_dysect.hpp
// 官方是多候选 cuckoo、按顺序增长；本实现固定单子表归属，按该表负载增长。
// insert 保留原实现允许重复键的语义，不把它改成 upsert。
template<typename Key, typename Value>
class sharehash_subtable {
public:
    struct bucketInfo {
        int begin_index = 0;
        int capacity = 0;
        int counter = 0;
    };

    struct bucketItem {
        Key key;
        Value value;
        bucketItem(const Key& k, const Value& v) : key(k), value(v) {}
    };

    using bitmap_t = std::uint8_t;

    // [V2-A 1] a_* 的单位是 bytes / 已存储记录；保留原重复键插入语义。
    // additive_bytes 为 b；默认 max() 表示自动取初始总存储，解决 n=0 启动问题。
    // 显式 b（例如1024）严格采用，不足初始化时拒绝，不静默放宽预算。
    struct SpaceBudget {
        std::size_t physical_limit_bytes = std::numeric_limits<std::size_t>::max();
        double a_stable_factor = 32.0;
        double a_migration_factor = 64.0;
        std::size_t current_live_bytes = 0;
        std::size_t live_keys_main = 0;
        std::size_t live_keys_fallback = 0; // V2-A 无后备区，始终为0。
        std::size_t additive_bytes = std::numeric_limits<std::size_t>::max();
        std::size_t peak_live_bytes = 0; // 分配前保守预留的历史高水位。
    };

    // [子表化修改2] 每个 topIndex 独占四组数组、有效元素计数和表级锁。
    // 表级锁位于稳定目录中；resize 只替换数组，不移动锁或其他子表。
    struct topIndex {
        int item_num = 0;
        int bucket_num = 0;
        bucketInfo* buckets = nullptr;
        bucketItem* bucket_items = nullptr;
        bitmap_t* none_bitmap = nullptr;
        SpinLock* locks_ = nullptr;
        std::size_t element_count = 0;
        std::size_t resize_count = 0;
        std::size_t migrated_items = 0;
        mutable SpinLock resize_lock_;

        topIndex() = default;
        topIndex(const topIndex&) = delete;
        topIndex& operator=(const topIndex&) = delete;

        ~topIndex() {
            // 只构造已占用槽中的对象；异常或迁移失败也能完整析构候选表。
            std::allocator<bucketItem> alloc;
            if (bucket_items && none_bitmap) {
                for (int i = 0; i < item_num; ++i) {
                    if ((none_bitmap[i / 8] & (bitmap_t(1) << (i % 8))) == 0)
                        std::allocator_traits<std::allocator<bucketItem>>::destroy(
                            alloc, bucket_items + i);
                }
            }
            if (bucket_items) alloc.deallocate(bucket_items, static_cast<std::size_t>(item_num));
            destroy_array(buckets, static_cast<std::size_t>(bucket_num));
            destroy_array(none_bitmap, bitmap_bytes(static_cast<std::size_t>(item_num)));
            destroy_array(locks_, static_cast<std::size_t>(item_num));
        }
    };

    // 只读快照用于验证局部扩容；地址不提供写权限，也不能跨 resize 解引用。
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
        search_range = 500,
        default_initial_buckets = 5000,
        auto_min_buckets_per_subtable = 128,
        auto_max_subtables = 256
    };

    class LockGuard {
        SpinLock& lock_;
    public:
        explicit LockGuard(SpinLock& lock) : lock_(lock) { lock_.lock(); }
        ~LockGuard() { lock_.unlock(); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    };

    // [V2-A 2a] 所有数组用 allocator 的精确元素分配，避免 new[] 的隐藏 cookie
    // 不在 sizeof(T)*count 中；分配器内部取整/元数据仍不属于此字节模型。
    template<class T> static T* allocate_array(std::size_t count) {
        std::allocator<T> alloc;
        T* result = alloc.allocate(count);
        std::size_t constructed = 0;
        try {
            for (; constructed < count; ++constructed)
                std::allocator_traits<std::allocator<T>>::construct(alloc, result + constructed);
        } catch (...) {
            while (constructed)
                std::allocator_traits<std::allocator<T>>::destroy(alloc, result + --constructed);
            alloc.deallocate(result, count);
            throw;
        }
        return result;
    }
    template<class T> static void destroy_array(T* array, std::size_t count) {
        if (!array) return;
        std::allocator<T> alloc;
        for (std::size_t i = 0; i < count; ++i)
            std::allocator_traits<std::allocator<T>>::destroy(alloc, array + i);
        alloc.deallocate(array, count);
    }
    struct DirectoryDeleter {
        std::size_t count;
        DirectoryDeleter(std::size_t n = 0) : count(n) {}
        void operator()(topIndex* directory) const { destroy_array(directory, count); }
    };
    using Directory = std::unique_ptr<topIndex[], DirectoryDeleter>;
    Directory top_indexes_;
    std::size_t subtable_count_ = 0;
    std::size_t subtable_bits_ = 0;
    std::size_t initial_bucket_count_ = 0;
    double max_load_factor_ = 0.8;
    std::hash<Key> hash_func_;
    // [V2-A 2] 全局账本锁先于子表锁，避免两张表同时通过同一份剩余预算。
    // 严格记账版串行化写入/扩容，读取仍仅锁目标子表。
    mutable SpinLock budget_lock_;
    SpaceBudget budget_;
    double min_load_factor_ = 0.75;
    std::size_t total_slot_capacity_ = 0;

    static std::size_t checked_add(std::size_t a, std::size_t b) {
        if (a > std::numeric_limits<std::size_t>::max() - b)
            throw std::overflow_error("space budget addition overflow");
        return a + b;
    }
    static std::size_t checked_multiply(std::size_t a, std::size_t b) {
        if (b && a > std::numeric_limits<std::size_t>::max() / b)
            throw std::overflow_error("space budget multiplication overflow");
        return a * b;
    }
    static SpaceBudget default_budget(std::size_t hard_limit_bytes) {
        SpaceBudget result;
        result.physical_limit_bytes = hard_limit_bytes;
        return result;
    }
    std::size_t allowed_bytes(double factor, std::size_t main_keys) const {
        // 先饱和再转整数，避免 a*n+b 浮点越界转换和整数回绕。
        const std::size_t maximum = std::numeric_limits<std::size_t>::max();
        const long double scaled = static_cast<long double>(factor) * main_keys;
        const std::size_t remaining = maximum - budget_.additive_bytes;
        if (scaled >= static_cast<long double>(remaining))
            return std::min(budget_.physical_limit_bytes, maximum);
        return std::min(budget_.physical_limit_bytes,
                        budget_.additive_bytes + static_cast<std::size_t>(scaled));
    }
    static std::size_t table_storage_bytes(std::size_t buckets) {
        const std::size_t slots = checked_multiply(buckets, bucket_size);
        std::size_t bytes = sizeof(topIndex);
        bytes = checked_add(bytes, checked_multiply(buckets, sizeof(bucketInfo)));
        bytes = checked_add(bytes, checked_multiply(slots, sizeof(bucketItem)));
        bytes = checked_add(bytes, checked_multiply(slots, sizeof(SpinLock)));
        return checked_add(bytes, bitmap_bytes(slots));
    }
    std::size_t initial_directory_bytes() const {
        std::size_t bytes = 0;
        const std::size_t quotient = initial_bucket_count_ / subtable_count_;
        const std::size_t remainder = initial_bucket_count_ % subtable_count_;
        for (std::size_t i = 0; i < subtable_count_; ++i)
            bytes = checked_add(bytes, table_storage_bytes(quotient + (i < remainder ? 1 : 0)));
        return bytes;
    }
    // [V2-A 3] 峰值要加完整新子表：旧表仍然存活，不能只加新旧差值。
    std::size_t projected_new_table_bytes(const topIndex& table) const {
        const std::size_t new_slots = checked_multiply(static_cast<std::size_t>(table.item_num), 2);
        return table_storage_bytes(new_slots / bucket_size);
    }
    std::size_t projected_peak_bytes(const topIndex& table) const {
        return checked_add(budget_.current_live_bytes, projected_new_table_bytes(table));
    }

    // 先预留全部候选存储。声明在候选对象之前，确保候选析构后才归还账本。
    class BudgetReservation {
        sharehash_subtable& owner_;
        std::size_t release_bytes_;
    public:
        BudgetReservation(sharehash_subtable& owner, std::size_t bytes)
            : owner_(owner), release_bytes_(bytes) {
            const std::size_t reserved = checked_add(owner_.budget_.current_live_bytes, bytes);
            owner_.budget_.current_live_bytes = reserved;
            owner_.budget_.peak_live_bytes = std::max(owner_.budget_.peak_live_bytes, reserved);
        }
        ~BudgetReservation() { owner_.budget_.current_live_bytes -= release_bytes_; }
        void release_old_on_commit(std::size_t bytes) { release_bytes_ = bytes; }
        BudgetReservation(const BudgetReservation&) = delete;
        BudgetReservation& operator=(const BudgetReservation&) = delete;
    };

    // 调用方已持有 budget_lock_ 和当前表锁；所有扩容入口都经过这里。
    bool try_resize_with_budget_locked(std::size_t id, topIndex& table) {
        if (static_cast<std::size_t>(table.bucket_num) >
            static_cast<std::size_t>(std::numeric_limits<int>::max()) / (2 * bucket_size))
            return false;
        try {
            const std::size_t new_bytes = projected_new_table_bytes(table);
            const std::size_t peak = projected_peak_bytes(table);
            if (peak > allowed_bytes(budget_.a_migration_factor, budget_.live_keys_main))
                return false;
            const std::size_t old_bytes = table_storage_bytes(table.bucket_num);
            const std::size_t stable = checked_add(budget_.current_live_bytes - old_bytes, new_bytes);
            if (stable > allowed_bytes(budget_.a_stable_factor, budget_.live_keys_main))
                return false;
            const std::size_t new_total_slots = checked_add(total_slot_capacity_, table.item_num);
            // min_load_factor 是全表扩容后的负载；单表翻倍后通常约0.4，不能按单表检查0.75。
            if (static_cast<long double>(budget_.live_keys_main) <
                static_cast<long double>(min_load_factor_) * new_total_slots)
                return false;
            return resize_locked(id, table);
        } catch (const std::overflow_error&) {
            return false;
        }
    }

    static std::size_t bitmap_bytes(std::size_t slots) { return (slots + 7) / 8; }
    static bool empty_slot(const topIndex& table, int slot) {
        return (table.none_bitmap[slot / 8] & (bitmap_t(1) << (slot % 8))) != 0;
    }
    static void mark_empty(topIndex& table, int slot) {
        table.none_bitmap[slot / 8] |= bitmap_t(1) << (slot % 8);
    }
    static void mark_used(topIndex& table, int slot) {
        table.none_bitmap[slot / 8] &= bitmap_t(~(bitmap_t(1) << (slot % 8)));
    }

    // [子表化修改3] T 生命周期内固定；只决定初始目录，不随局部 resize 改变。
    // [V2-A 4] 自动 T = floor_pow2(min(256, max(1, total_initial_buckets / 128))).
    // 默认 5000 桶 -> T=32：8*157 + 24*156；总初始槽数仍为20000。
    static std::size_t choose_subtable_count(std::size_t requested, std::size_t buckets) {
        if (buckets == 0 || buckets > static_cast<std::size_t>(std::numeric_limits<int>::max()) / bucket_size)
            throw std::invalid_argument("initial bucket count is outside the supported range");
        if (requested != 0) {
            if ((requested & (requested - 1)) != 0 || requested > buckets)
                throw std::invalid_argument("T must be a power of two and no greater than initial buckets");
            return requested;
        }
        const std::size_t limit = std::min<std::size_t>(
            auto_max_subtables, std::max<std::size_t>(1, buckets / auto_min_buckets_per_subtable));
        std::size_t result = 1;
        while (result <= limit / 2) result *= 2;
        return result;
    }

    // [子表化修改4] 先做 SplitMix64 混合，再把选表位与表内桶位分离。
    // 避免 std::hash<uint64_t> 的低位同时选表/选桶而造成相关性。
    // sid = mixed & (T-1); home = (mixed >> log2(T)) % table.bucket_num。
    std::uint64_t mixed_hash(const Key& key) const {
        std::uint64_t hash = static_cast<std::uint64_t>(hash_func_(key)) + UINT64_C(0x9e3779b97f4a7c15);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        return hash ^ (hash >> 31);
    }
    std::size_t subtable_id(std::uint64_t hash) const { return hash & (subtable_count_ - 1); }
    std::uint64_t local_hash(std::uint64_t hash) const { return hash >> subtable_bits_; }
    static int home_bucket(const topIndex& table, std::uint64_t hash) {
        return static_cast<int>(hash % static_cast<std::uint64_t>(table.bucket_num));
    }

    static void allocate_storage(topIndex& table, std::size_t buckets) {
        if (buckets == 0 || buckets > static_cast<std::size_t>(std::numeric_limits<int>::max()) / bucket_size)
            throw std::length_error("subtable capacity exceeds the integer slot limit");
        table.bucket_num = static_cast<int>(buckets);
        table.item_num = static_cast<int>(buckets * bucket_size);
        table.buckets = allocate_array<bucketInfo>(buckets);
        table.none_bitmap = allocate_array<bitmap_t>(bitmap_bytes(table.item_num));
        std::memset(table.none_bitmap, 0xff, bitmap_bytes(table.item_num));
        table.locks_ = allocate_array<SpinLock>(static_cast<std::size_t>(table.item_num));
        std::allocator<bucketItem> alloc;
        table.bucket_items = alloc.allocate(static_cast<std::size_t>(table.item_num));
        for (int i = 0; i < table.bucket_num; ++i) {
            table.buckets[i].begin_index = i * static_cast<int>(bucket_size);
            table.buckets[i].capacity = static_cast<int>(bucket_size);
        }
    }

    static int free_slot(const topIndex& table, int bucket_id) {
        const bucketInfo& bucket = table.buckets[bucket_id];
        if (bucket.counter >= bucket.capacity) return -1;
        for (int i = bucket.begin_index; i < bucket.begin_index + bucket.capacity; ++i)
            if (empty_slot(table, i)) return i;
        return -1;
    }

    static void construct_slot(topIndex& table, int slot, const Key& key, const Value& value) {
        std::allocator<bucketItem> alloc;
        std::allocator_traits<std::allocator<bucketItem>>::construct(
            alloc, table.bucket_items + slot, key, value);
        mark_used(table, slot);
    }
    static void destroy_slot(topIndex& table, int slot) {
        std::allocator<bucketItem> alloc;
        std::allocator_traits<std::allocator<bucketItem>>::destroy(alloc, table.bucket_items + slot);
        mark_empty(table, slot);
    }

    // [子表化修改5] 借位保留修复后的“右桶内部搬移，再传递空槽”不变量。
    // home、SEARCHRANGE 和左右边界均限定在当前子表，不向相邻子表借位。
    static bool insert_without_growth(topIndex& table, const Key& key, const Value& value,
                                      std::uint64_t hash) {
        const int home = home_bucket(table, hash);
        int slot = free_slot(table, home);
        if (slot < 0) {
            int donor = home + 1;
            for (; donor < table.bucket_num && donor - home < static_cast<int>(search_range); ++donor) {
                slot = free_slot(table, donor);
                if (slot >= 0) break;
            }
            if (slot < 0) return false;
            for (int current = donor; current > home; --current) {
                bucketInfo& right = table.buckets[current];
                bucketInfo& left = table.buckets[current - 1];
                const int first = right.begin_index;
                // slot 是当前右桶中的空位；零容量中间桶在接收后也能继续传递。
                LockGuard first_guard(table.locks_[first]);
                if (slot != first) {
                    LockGuard slot_guard(table.locks_[slot]);
                    if (!empty_slot(table, first)) {
                        construct_slot(table, slot, table.bucket_items[first].key,
                                       table.bucket_items[first].value);
                        destroy_slot(table, first);
                    }
                }
                ++right.begin_index;
                --right.capacity;
                ++left.capacity;
                slot = first;
            }
        }
        LockGuard slot_guard(table.locks_[slot]);
        construct_slot(table, slot, key, value);
        ++table.buckets[home].counter;
        ++table.element_count;
        return true;
    }

    // [子表化修改6] 候选表完全迁移成功后才交换四组数组；失败直接析构候选表。
    // 原表及其余 T-1 张表保持可查询。候选迁移不递归增长、不获取原表锁。
    bool resize_locked(std::size_t id, topIndex& current) {
        const std::size_t old_buckets = static_cast<std::size_t>(current.bucket_num);
        if (old_buckets > static_cast<std::size_t>(std::numeric_limits<int>::max()) / (2 * bucket_size))
            return false;
        const std::size_t old_bytes = table_storage_bytes(old_buckets);
        const std::size_t old_slots = static_cast<std::size_t>(current.item_num);
        BudgetReservation reservation(*this, projected_new_table_bytes(current));
        topIndex candidate;
        try {
            allocate_storage(candidate, old_buckets * 2);
            for (int i = 0; i < current.item_num; ++i) {
                if (empty_slot(current, i)) continue;
                const bucketItem& item = current.bucket_items[i];
                const std::uint64_t hash = mixed_hash(item.key);
                if (subtable_id(hash) != id ||
                    !insert_without_growth(candidate, item.key, item.value, local_hash(hash)))
                    return false;
            }
        } catch (const std::bad_alloc&) {
            return false;
        }
        const std::size_t moved = current.element_count;
        using std::swap;
        swap(current.bucket_num, candidate.bucket_num);
        swap(current.item_num, candidate.item_num);
        swap(current.buckets, candidate.buckets);
        swap(current.bucket_items, candidate.bucket_items);
        swap(current.none_bitmap, candidate.none_bitmap);
        swap(current.locks_, candidate.locks_);
        swap(current.element_count, candidate.element_count);
        ++current.resize_count;
        current.migrated_items += moved;
        total_slot_capacity_ += old_slots;
        // candidate 现在持有旧数组；其析构后，从账本扣除旧数组与旧候选元数据。
        reservation.release_old_on_commit(old_bytes);
        return true; // candidate 析构释放本次被替换的旧表；不触及其他表。
    }

    topIndex& checked_subtable(std::size_t id) const {
        if (id >= subtable_count_) throw std::out_of_range("subtable id is out of range");
        if (!top_indexes_) throw std::logic_error("ShareHash storage has been released");
        return top_indexes_[id];
    }

public:
    // [V2-A 5] 保留前三个参数；追加全局最小负载与物理字节上限。
    // 如需配置 a_stable/a_migration/b，使用第五个参数为 SpaceBudget 的重载。
    explicit sharehash_subtable(std::size_t T = 0, double max_load_factor = 0.8,
                               std::size_t total_initial_buckets = default_initial_buckets,
                               double min_load_factor = 0.75,
                               std::size_t hard_limit_bytes = std::numeric_limits<std::size_t>::max())
        : sharehash_subtable(T, max_load_factor, total_initial_buckets, min_load_factor,
                            default_budget(hard_limit_bytes)) {}

    sharehash_subtable(std::size_t T, double max_load_factor,
                      std::size_t total_initial_buckets, double min_load_factor,
                      const SpaceBudget& configured_budget)
        : subtable_count_(choose_subtable_count(T, total_initial_buckets)),
          initial_bucket_count_(total_initial_buckets), max_load_factor_(max_load_factor),
          budget_(configured_budget), min_load_factor_(min_load_factor) {
        // sizeof(bucketItem) 无法覆盖 string 等内部动态分配，因此严格版限定普通记录。
        static_assert(std::is_trivially_copyable<Key>::value &&
                      std::is_trivially_copyable<Value>::value,
                      "V2-A byte accounting requires trivially copyable keys and values");
        if (!std::isfinite(max_load_factor_) || max_load_factor_ <= 0 || max_load_factor_ > 1)
            throw std::invalid_argument("max_load_factor must be finite and in (0,1]");
        if (!std::isfinite(min_load_factor_) || min_load_factor_ < 0 ||
            min_load_factor_ > max_load_factor_)
            throw std::invalid_argument("min_load_factor must be finite and in [0,max_load_factor]");
        if (!std::isfinite(budget_.a_stable_factor) || budget_.a_stable_factor < 0 ||
            !std::isfinite(budget_.a_migration_factor) || budget_.a_migration_factor < 0)
            throw std::invalid_argument("space factors must be finite and nonnegative");
        for (std::size_t count = subtable_count_; count > 1; count >>= 1) ++subtable_bits_;
        // 外部只能配置限额；实时字段必须由本实例初始化，不能信任传入的计数。
        budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = 0;
        budget_.live_keys_fallback = 0;
        budget_.peak_live_bytes = sizeof(*this);
        if (budget_.additive_bytes == std::numeric_limits<std::size_t>::max())
            budget_.additive_bytes = checked_add(sizeof(*this), initial_directory_bytes());
        initShareHash();
    }
    ~sharehash_subtable() = default;
    sharehash_subtable(const sharehash_subtable&) = delete;
    sharehash_subtable& operator=(const sharehash_subtable&) = delete;
    sharehash_subtable(sharehash_subtable&&) = delete;
    sharehash_subtable& operator=(sharehash_subtable&&) = delete;

    // [V2-A 6] 初始化和重置也检查新旧目录共存峰值，不能绕过硬上限。
    // 与原版相同：重置、释放和析构不能与其他操作并发。
    void initShareHash() {
        LockGuard budget_guard(budget_lock_);
        const std::size_t directory_bytes = initial_directory_bytes();
        const std::size_t peak = checked_add(budget_.current_live_bytes, directory_bytes);
        const std::size_t final_bytes = checked_add(sizeof(*this), directory_bytes);
        if (peak > allowed_bytes(budget_.a_migration_factor, budget_.live_keys_main) ||
            final_bytes > allowed_bytes(budget_.a_stable_factor, 0))
            throw std::length_error("initialization or reset exceeds the space budget");
        const std::size_t old_directory_bytes = budget_.current_live_bytes - sizeof(*this);
        BudgetReservation reservation(*this, directory_bytes);
        Directory replacement(allocate_array<topIndex>(subtable_count_), DirectoryDeleter(subtable_count_));
        const std::size_t quotient = initial_bucket_count_ / subtable_count_;
        const std::size_t remainder = initial_bucket_count_ % subtable_count_;
        for (std::size_t i = 0; i < subtable_count_; ++i)
            allocate_storage(replacement[i], quotient + (i < remainder ? 1 : 0));
        top_indexes_.swap(replacement);
        replacement.reset(); // 先释放旧目录，之后 reservation 才归还旧存储的记账。
        reservation.release_old_on_commit(old_directory_bytes);
        total_slot_capacity_ = initial_bucket_count_ * bucket_size;
        budget_.live_keys_main = 0;
        budget_.live_keys_fallback = 0;
    }
    void deleteShareHash() {
        LockGuard budget_guard(budget_lock_);
        top_indexes_.reset();
        total_slot_capacity_ = 0;
        budget_.current_live_bytes = sizeof(*this);
        budget_.live_keys_main = budget_.live_keys_fallback = 0;
    }

    bool get(const Key& key, Value& value) const {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        topIndex& table = top_indexes_[subtable_id(hash)];
        LockGuard table_guard(table.resize_lock_);
        const bucketInfo& bucket = table.buckets[home_bucket(table, local_hash(hash))];
        for (int i = bucket.begin_index; i < bucket.begin_index + bucket.capacity; ++i) {
            if (!empty_slot(table, i)) {
                LockGuard slot_guard(table.locks_[i]);
                if (table.bucket_items[i].key == key) {
                    value = table.bucket_items[i].value;
                    return true;
                }
            }
        }
        return false;
    }

    // [子表化修改7] 写入前检查目标子表的投影负载 (size+1)/capacity。
    // 超过阈值仅 resize_locked(id)。借位失败仍保留原局部增长兜底。
    // 同一次写入因借位失败最多额外增长4次，避免病态碰撞导致无限倍增。
    bool insert(const Key& key, const Value& value) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::size_t id = subtable_id(hash);
        LockGuard budget_guard(budget_lock_);
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        // 成功写入必须满足稳定预算；删除不缩容后也不能靠插入绕过此检查。
        if (budget_.live_keys_main == std::numeric_limits<std::size_t>::max() ||
            budget_.current_live_bytes > allowed_bytes(budget_.a_stable_factor,
                                                       budget_.live_keys_main + 1))
            return false;
        while (static_cast<long double>(table.element_count) + 1 >
               static_cast<long double>(max_load_factor_) * table.item_num) {
            if (!try_resize_with_budget_locked(id, table)) return false;
        }
        // [插入重试修复] 原有负载阈值检查不变；借位失败最多额外扩容4次。
        // 每次仍只扩容并重试当前子表，避免末桶连续冲突时过早返回 false。
        constexpr unsigned max_growth_attempts = 4;
        for (unsigned growths = 0; ; ++growths) {
            if (insert_without_growth(table, key, value, local_hash(hash))) {
                ++budget_.live_keys_main;
                return true;
            }

            if (growths == max_growth_attempts)
                return false;

            // 仍持有当前子表锁，只扩容这一张子表。
            if (!try_resize_with_budget_locked(id, table))
                return false;
        }
    }

    bool remove(const Key& key) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        LockGuard budget_guard(budget_lock_);
        topIndex& table = top_indexes_[subtable_id(hash)];
        LockGuard table_guard(table.resize_lock_);
        bucketInfo& bucket = table.buckets[home_bucket(table, local_hash(hash))];
        for (int i = bucket.begin_index; i < bucket.begin_index + bucket.capacity; ++i) {
            if (!empty_slot(table, i)) {
                LockGuard slot_guard(table.locks_[i]);
                if (table.bucket_items[i].key == key) {
                    destroy_slot(table, i);
                    --bucket.counter;
                    --table.element_count;
                    --budget_.live_keys_main; // V2-A不缩容；后续分配按新的 n 重新准入。
                    return true;
                }
            }
        }
        return false;
    }

    // [V2-A 7] 手动 resize 与自动扩容共用准入，避免显式调用绕开限制。
    bool try_resize_with_budget(std::size_t id) {
        if (!top_indexes_ || id >= subtable_count_) return false;
        LockGuard budget_guard(budget_lock_);
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        return try_resize_with_budget_locked(id, table);
    }
    bool resize(std::size_t id = 0) { return try_resize_with_budget(id); }

    SpaceBudget getSpaceBudget() const {
        LockGuard guard(budget_lock_);
        return budget_;
    }
    double min_load_factor() const noexcept { return min_load_factor_; }
    std::size_t projected_new_table_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_);
        topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_);
        return projected_new_table_bytes(table);
    }
    std::size_t projected_peak_bytes(std::size_t id) const {
        LockGuard budget_guard(budget_lock_);
        topIndex& table = checked_subtable(id);
        LockGuard table_guard(table.resize_lock_);
        return projected_peak_bytes(table);
    }

    // [子表化修改8] 总容量/内存汇总所有子表；新增只读分表计数与快照。
    std::size_t getSubtableCount() const noexcept { return subtable_count_; }
    std::size_t getSubtableId(const Key& key) const { return subtable_id(mixed_hash(key)); }
    std::size_t getSubtableBucketForKey(const Key& key) const {
        const std::uint64_t hash = mixed_hash(key);
        topIndex& table = checked_subtable(subtable_id(hash));
        LockGuard guard(table.resize_lock_);
        return static_cast<std::size_t>(home_bucket(table, local_hash(hash)));
    }
    double max_load_factor() const noexcept { return max_load_factor_; }
    SubtableStats getSubtableStats(std::size_t id) const {
        topIndex& table = checked_subtable(id);
        LockGuard guard(table.resize_lock_);
        return {static_cast<std::size_t>(table.bucket_num), static_cast<std::size_t>(table.item_num),
                table.element_count, table.resize_count, table.migrated_items, table.buckets,
                table.bucket_items, table.none_bitmap, table.locks_};
    }
    std::size_t getSubtableItemNumber(std::size_t id) const { return getSubtableStats(id).item_num; }
    std::size_t getSubtableBucketNumber(std::size_t id) const { return getSubtableStats(id).bucket_num; }
    std::size_t getSubtableSize(std::size_t id) const { return getSubtableStats(id).element_count; }
    std::size_t getSubtableResizeCount(std::size_t id) const { return getSubtableStats(id).resize_count; }

    // [V2-A 8] 聚合统计从全局账本读取，包含控制块和锁，不采用基准的扣锁口径。
    std::size_t size() const { return getSpaceBudget().live_keys_main; }
    long long getBucketNumber() const { return getItemNumber() / static_cast<long long>(bucket_size); }
    long long getItemNumber() const {
        LockGuard guard(budget_lock_);
        if (total_slot_capacity_ > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            throw std::overflow_error("slot capacity does not fit long long");
        return static_cast<long long>(total_slot_capacity_);
    }
    long long memoryUsage() const {
        const std::size_t bytes = getSpaceBudget().current_live_bytes;
        if (bytes > static_cast<std::size_t>(std::numeric_limits<long long>::max()))
            throw std::overflow_error("memory usage does not fit long long");
        return static_cast<long long>(bytes);
    }
};

} // namespace ShareHash

#endif // SHAREHASH_SHAREHASH_SUBTABLE_SPACELIMIT_A_H
