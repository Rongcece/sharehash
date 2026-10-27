// Subtable variant of ShareHash. All implementation changes live in this header.
#ifndef SHAREHASH_SHAREHASH_SUBTABLE_H
#define SHAREHASH_SHAREHASH_SUBTABLE_H

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
            delete[] buckets;
            delete[] none_bitmap;
            delete[] locks_;
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
        auto_min_buckets_per_subtable = 256,
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

    std::unique_ptr<topIndex[]> top_indexes_;
    std::size_t subtable_count_ = 0;
    std::size_t subtable_bits_ = 0;
    std::size_t initial_bucket_count_ = 0;
    double max_load_factor_ = 0.8;
    std::hash<Key> hash_func_;

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
    // 自动 T = floor_pow2(min(256, max(1, total_initial_buckets / 256))).
    // 默认 5000 桶 -> T=16；商余数分配为 8*313 + 8*312，总槽数仍为 20000。
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
        table.buckets = new bucketInfo[buckets];
        table.none_bitmap = new bitmap_t[bitmap_bytes(table.item_num)];
        std::memset(table.none_bitmap, 0xff, bitmap_bytes(table.item_num));
        table.locks_ = new SpinLock[table.item_num];
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
        return true; // candidate 析构释放本次被替换的旧表；不触及其他表。
    }

    topIndex& checked_subtable(std::size_t id) const {
        if (id >= subtable_count_) throw std::out_of_range("subtable id is out of range");
        if (!top_indexes_) throw std::logic_error("ShareHash storage has been released");
        return top_indexes_[id];
    }

public:
    // T=0 自动选择；显式 T 为不超过总初始桶数的2幂。alpha 可取 (0,1]。
    explicit sharehash_subtable(std::size_t T = 0, double max_load_factor = 0.8,
                               std::size_t total_initial_buckets = default_initial_buckets)
        : subtable_count_(choose_subtable_count(T, total_initial_buckets)),
          initial_bucket_count_(total_initial_buckets), max_load_factor_(max_load_factor) {
        if (!std::isfinite(max_load_factor_) || max_load_factor_ <= 0 || max_load_factor_ > 1)
            throw std::invalid_argument("max_load_factor must be finite and in (0,1]");
        for (std::size_t count = subtable_count_; count > 1; count >>= 1) ++subtable_bits_;
        initShareHash();
    }
    ~sharehash_subtable() = default;
    sharehash_subtable(const sharehash_subtable&) = delete;
    sharehash_subtable& operator=(const sharehash_subtable&) = delete;
    sharehash_subtable(sharehash_subtable&&) = delete;
    sharehash_subtable& operator=(sharehash_subtable&&) = delete;

    // 重置/显式释放与析构须由调用方保证没有并发操作；普通操作只锁目标子表。
    void initShareHash() {
        std::unique_ptr<topIndex[]> replacement(new topIndex[subtable_count_]);
        const std::size_t quotient = initial_bucket_count_ / subtable_count_;
        const std::size_t remainder = initial_bucket_count_ % subtable_count_;
        for (std::size_t i = 0; i < subtable_count_; ++i)
            allocate_storage(replacement[i], quotient + (i < remainder ? 1 : 0));
        top_indexes_.swap(replacement);
    }
    void deleteShareHash() { top_indexes_.reset(); }

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
    // 同一次写入至多因借位失败再增长一次，避免病态碰撞导致无限倍增。
    bool insert(const Key& key, const Value& value) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
        const std::size_t id = subtable_id(hash);
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        while (static_cast<long double>(table.element_count) + 1 >
               static_cast<long double>(max_load_factor_) * table.item_num) {
            if (!resize_locked(id, table)) return false;
        }
        if (insert_without_growth(table, key, value, local_hash(hash))) return true;
        if (!resize_locked(id, table)) return false;
        return insert_without_growth(table, key, value, local_hash(hash));
    }

    bool remove(const Key& key) {
        if (!top_indexes_) return false;
        const std::uint64_t hash = mixed_hash(key);
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
                    return true;
                }
            }
        }
        return false;
    }

    // 显式 resize 也仅作用于一个子表；无参形式固定作用于子表0，不全表增长。
    bool resize(std::size_t id = 0) {
        if (!top_indexes_ || id >= subtable_count_) return false;
        topIndex& table = top_indexes_[id];
        LockGuard table_guard(table.resize_lock_);
        return resize_locked(id, table);
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

    // 聚合操作逐表取快照；并发写入时不承诺所有子表属于同一时刻。
    std::size_t size() const {
        std::size_t count = 0;
        if (top_indexes_)
            for (std::size_t i = 0; i < subtable_count_; ++i) count += getSubtableSize(i);
        return count;
    }
    long long getBucketNumber() const {
        long long count = 0;
        if (top_indexes_)
            for (std::size_t i = 0; i < subtable_count_; ++i) count += getSubtableBucketNumber(i);
        return count;
    }
    long long getItemNumber() const {
        long long count = 0;
        if (top_indexes_)
            for (std::size_t i = 0; i < subtable_count_; ++i) count += getSubtableItemNumber(i);
        return count;
    }
    long long memoryUsage() const {
        long long bytes = sizeof(*this);
        if (!top_indexes_) return bytes;
        bytes += static_cast<long long>(subtable_count_ * sizeof(topIndex));
        for (std::size_t i = 0; i < subtable_count_; ++i) {
            const auto table = getSubtableStats(i);
            bytes += static_cast<long long>(table.bucket_num * sizeof(bucketInfo));
            bytes += static_cast<long long>(table.item_num * sizeof(bucketItem));
            bytes += static_cast<long long>(table.item_num * sizeof(SpinLock));
            bytes += static_cast<long long>(bitmap_bytes(table.item_num));
        }
        return bytes; // 与原口径一致：分配大小估算，不等同 RSS，不含分配器元数据。
    }
};

} // namespace ShareHash

#endif // SHAREHASH_SHAREHASH_SUBTABLE_H
