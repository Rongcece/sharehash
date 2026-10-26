//
// Created by yyx on 2024/1/2.
//

#ifndef SHAREHASH_SHAREHASH_H
#define SHAREHASH_SHAREHASH_H

#include <iostream>
#include <cstring>
#include <functional>
#include "murmur3.h"
#include "concurrency.h"

namespace ShareHash {

    #define SHAREBUCKETSIZE 4
    #define BUCKETNUMBER 5000
    #define SEARCHRANGE 500

    typedef uint8_t bitmap_t;
    #define BITMAP_WIDTH (sizeof(bitmap_t) * 8)
    #define BITMAP_SIZE(num_items) (((num_items) + BITMAP_WIDTH - 1) / BITMAP_WIDTH)
    #define BITMAP_GET(bitmap, pos) (((bitmap)[(pos) / BITMAP_WIDTH] >> ((pos) % BITMAP_WIDTH)) & 1)
    #define BITMAP_SET(bitmap, pos) ((bitmap)[(pos) / BITMAP_WIDTH] |= 1 << ((pos) % BITMAP_WIDTH))
    #define BITMAP_CLEAR(bitmap, pos) ((bitmap)[(pos) / BITMAP_WIDTH] &= ~bitmap_t(1 << ((pos) % BITMAP_WIDTH)))

    template<typename Key, typename Value>
    class sharehash {

    public:
        //顶层索引项
        struct bucketInfo {
            int begin_index;
            int capacity;
            int counter;
        };

        //顶层索引
        struct topIndex {
            int item_num;
            int bucket_num;
            int f_seed;
            struct bucketInfo *buckets;
            bitmap_t *none_bitmap;
            SpinLock *locks_;
        };

        //桶内元素项
        struct bucketItem {
            Key key;
            Value value;
        };

        bucketInfo *bucketInfo_allocator(int num) {
            std::allocator<bucketInfo> alloc;
            bucketInfo *p = nullptr;
            p = alloc.allocate(num);
            return p;
        }

        void bucketInfo_deallocator(bucketInfo *p, int num) {
            std::allocator<bucketInfo> dealloc;
            dealloc.deallocate(p, num);
        }

        topIndex *topIndex_allocator(int num) {
            std::allocator<topIndex> alloc;
            topIndex *p = nullptr;
            p = alloc.allocate(num);
            return p;
        }

        void topIndex_deallocator(topIndex *p, int num) {
            std::allocator<topIndex> dealloc;
            dealloc.deallocate(p, num);
        }

        bucketItem *bucketItem_allocator(int num) {
            std::allocator<bucketItem> alloc;
            bucketItem *p = nullptr;
            p = alloc.allocate(num);
            return p;
        }

        void bucketItem_deallocator(bucketItem *p, int num) {
            std::allocator<bucketItem> dealloc;
            dealloc.deallocate(p, num);
        }

        bitmap_t *bitmap_allocator(int num) {
            std::allocator<bitmap_t> alloc;
            bitmap_t *p = alloc.allocate(num);
            return p;
        }

        void bitmap_deallocator(bitmap_t *p, int num) {
            std::allocator<bitmap_t> dealloc;
            dealloc.deallocate(p, num);
        }

        SpinLock *spinlock_allocator(int num) {
            std::allocator<SpinLock> alloc;
            SpinLock *p = alloc.allocate(num);
            return p;
        }

        void spinlock_deallocator(SpinLock *p, int num) {
            std::allocator<SpinLock> dealloc;
            dealloc.deallocate(p, num);
        }

    private:
        struct topIndex *top_index;
        struct bucketItem *bucket_items;
        std::hash<Key> hash_func;
        SpinLock resize_lock_; // 添加全局的resize锁

    public:
        //初始化哈希表
        void initShareHash() {
            top_index = topIndex_allocator(1);
            top_index->item_num = BUCKETNUMBER * SHAREBUCKETSIZE;
            top_index->bucket_num = BUCKETNUMBER;
            top_index->buckets = bucketInfo_allocator(BUCKETNUMBER);
            top_index->f_seed = rand() % MAX_PRIME32;
            top_index->none_bitmap = bitmap_allocator(BITMAP_SIZE(top_index->item_num));
            memset(top_index->none_bitmap, 0xff, sizeof(bitmap_t) * BITMAP_SIZE(top_index->item_num));
            top_index->locks_ = new SpinLock[top_index->item_num]; // 每个元素一个锁
            bucket_items = bucketItem_allocator(BUCKETNUMBER * SHAREBUCKETSIZE);

            //清空桶数据
            for (int i = 0; i < BUCKETNUMBER; i++) {
                top_index->buckets[i].begin_index = i * SHAREBUCKETSIZE;
                top_index->buckets[i].capacity = SHAREBUCKETSIZE;
                top_index->buckets[i].counter = 0;
            }
        }

        // 删除哈希表
        void deleteShareHash(struct topIndex *delete_top_index = nullptr) {
            // 如果没有传入参数，使用当前的 top_index
            struct topIndex* current_delete_top_index = delete_top_index ? delete_top_index : this->top_index;
            struct bucketItem* items_to_delete = this->bucket_items; // 默认删除当前的 bucket_items

            // 仅当删除的是当前活动的 top_index 时，才操作 this->bucket_items
            // resize 函数会自行处理旧的 bucket_items
            if (current_delete_top_index == this->top_index && this->bucket_items != nullptr) {
                 bucketItem_deallocator(this->bucket_items, current_delete_top_index->item_num);
                 this->bucket_items = nullptr; // 防止重复释放
            } else if (delete_top_index && delete_top_index != this->top_index) {
                // 如果传入的是一个非当前活动的 top_index (例如，旧的 top_index 在 resize 后被清理)
                // 它的 bucket_items 应该由 resize 逻辑单独处理，这里不应释放 this->bucket_items
                // 此处假设 delete_top_index 的 bucket_items 已被正确处理或不需要在此处处理
            }

            if (current_delete_top_index) {
                if (current_delete_top_index->buckets) {
                    bucketInfo_deallocator(current_delete_top_index->buckets, current_delete_top_index->bucket_num);
                }
                if (current_delete_top_index->none_bitmap) {
                    bitmap_deallocator(current_delete_top_index->none_bitmap, BITMAP_SIZE(current_delete_top_index->item_num));
                }
                if (current_delete_top_index->locks_) {
                    delete[] current_delete_top_index->locks_;
                }
                topIndex_deallocator(current_delete_top_index, 1);
            }
        }

        sharehash() {
            initShareHash();
        }

        ~sharehash() {
            deleteShareHash();
        }

        bool get(const Key key, Value &value) {
            resize_lock_.lock(); // 获取 resize 锁

            // 获取锁后，检查核心数据结构是否有效
            if (!top_index || !bucket_items) {
                resize_lock_.unlock();
                // std::cerr << "Error in get: Hash table not properly initialized." << std::endl;
                return false;
            }

            int bucket_num = hash_func(key) % top_index->bucket_num;
            // 边界检查
            if (bucket_num < 0 || bucket_num >= top_index->bucket_num) {
                resize_lock_.unlock();
                return false; // 无效的桶号
            }
            struct bucketInfo *bucket = top_index->buckets + bucket_num;
            int item_index = bucket->begin_index;
            bool found = false;

            // 确保迭代范围有效
            if (item_index < 0 || item_index + bucket->capacity > top_index->item_num) {
                 resize_lock_.unlock();
                 return false; // 桶的索引范围无效
            }

            for (int i = item_index; i < bucket->capacity + item_index; i++) {
                // 再次检查索引 i 的有效性，以防 bucket->capacity 异常
                if (i < 0 || i >= top_index->item_num) continue;

                if (!BITMAP_GET(top_index->none_bitmap, i)) {
                    top_index->locks_[i].lock(); // 获取元素锁
                    // 再次检查位图和键值，因为在获取元素锁期间可能发生变化
                    if (!BITMAP_GET(top_index->none_bitmap, i) && bucket_items[i].key == key) {
                        value = bucket_items[i].value;
                        found = true;
                        top_index->locks_[i].unlock(); // 释放元素锁
                        break; // 找到元素，跳出循环
                    }
                    top_index->locks_[i].unlock(); // 释放元素锁
                }
            }

            resize_lock_.unlock(); // 释放 resize 锁
            return found;
        }

        bool insert(const Key key, const Value value, bool resize_insert = false) {
            if (!resize_insert) {
                resize_lock_.lock(); // 非 resize 插入操作，获取 resize 锁
            }

        retry:
            // 获取锁后（或在 resize_insert 模式下），检查核心数据结构
            if (!top_index || !bucket_items) {
                std::cerr << "Critical error: Invalid table structure in insert!" << std::endl;
                if (!resize_insert) {
                    resize_lock_.unlock();
                }
                return false;
            }

            int bucket_num = hash_func(key) % top_index->bucket_num;
            if (bucket_num < 0 || bucket_num >= top_index->bucket_num) {
                std::cerr << "Critical error: Invalid bucket number: " << bucket_num << std::endl;
                if (!resize_insert) {
                    resize_lock_.unlock();
                }
                return false;
            }

            struct bucketInfo *bucket = top_index->buckets + bucket_num;

            if (bucket->begin_index < 0 || bucket->begin_index + bucket->capacity > top_index->item_num) {
                std::cerr << "Critical error: Bucket range out of bounds: " << bucket->begin_index
                          << " to " << (bucket->begin_index + bucket->capacity)
                          << ", item_num: " << top_index->item_num << std::endl;
                if (!resize_insert) {
                    resize_lock_.unlock();
                }
                return false;
            }

            // 检查桶是否可用（即未满）
            if (checkBucket(bucket_num)) { // checkBucket 内部会访问 top_index，此时 resize_lock_ 已持有
                for(int i = bucket->begin_index; i < bucket->begin_index + bucket->capacity; i++) {
                    if (i < 0 || i >= top_index->item_num) { // 范围检查
                        std::cerr << "Critical error: Item index out of bounds in insert (direct): " << i
                                 << ", item_num: " << top_index->item_num << std::endl;
                        continue;
                    }
                    if (BITMAP_GET(top_index->none_bitmap, i)) {
                        top_index->locks_[i].lock(); // 获取元素锁
                        if (i < 0 || i >= top_index->item_num) { // 再次检查索引有效性
                             top_index->locks_[i].unlock();
                             std::cerr << "Critical error: Item index out of bounds (after lock): " << i << std::endl;
                             continue;
                        }
                        if (BITMAP_GET(top_index->none_bitmap, i)) { // 再次检查位图
                            BITMAP_CLEAR(top_index->none_bitmap, i);
                            bucket_items[i].key = key;
                            bucket_items[i].value = value;
                            bucket->counter++;
                            top_index->locks_[i].unlock(); // 释放元素锁
                            if (!resize_insert) {
                                resize_lock_.unlock(); // 释放 resize 锁
                            }
                            return true;
                        }
                        top_index->locks_[i].unlock(); // 释放元素锁
                    }
                }
            }

            // [修改1] 桶满时仍按原方向和 SEARCHRANGE 向右借位；修正下方空槽传递。
            int cur_bucket_num = bucket_num + 1; // 保持变量名与原代码一致
            int offset = 1;
            while (cur_bucket_num < top_index->bucket_num && offset < SEARCHRANGE) {
                if (checkBucket(cur_bucket_num)) {
                    int temp_target_bucket_for_eviction = cur_bucket_num; // 临时变量，避免与外层 cur_bucket_num 混淆语义
                    while (temp_target_bucket_for_eviction > bucket_num) {
                        bucketInfo *current_eviction_bucket = top_index->buckets + temp_target_bucket_for_eviction;
                        bucketInfo *previous_eviction_bucket = top_index->buckets + temp_target_bucket_for_eviction - 1;

                        // [修改1 BEGIN] 将右桶首槽腾空后交给左桶，保持键的所属桶不变。
                        int new_index = -1;
                        for (int k = 0; k < current_eviction_bucket->capacity; k++) {
                            if (current_eviction_bucket->begin_index + k >= top_index->item_num) break; // 越界检查
                            if (BITMAP_GET(top_index->none_bitmap, current_eviction_bucket->begin_index + k)) {
                                new_index = current_eviction_bucket->begin_index + k;
                                break;
                            }
                        }

                        // 原代码搬走左桶任意元素；现在只在右桶自己的区间内搬移首槽元素。
                        int old_index = current_eviction_bucket->begin_index;
                        if (current_eviction_bucket->capacity <= 0 || new_index < old_index ||
                            old_index < 0 || new_index >= top_index->item_num ||
                            previous_eviction_bucket->begin_index + previous_eviction_bucket->capacity != old_index) {
                            break; // 没有可传递的连续空槽，不跨过无效边界继续借位。
                        }

                        SpinLock *first_lock = &top_index->locks_[old_index];
                        // 首槽本来为空时 new_index == old_index，只获取一次锁。
                        SpinLock *second_lock = (new_index == old_index) ? nullptr : &top_index->locks_[new_index];
                        first_lock->lock();
                        if (second_lock) second_lock->lock();

                        bool transferred = false;
                        if (BITMAP_GET(top_index->none_bitmap, new_index)) {
                            if (!BITMAP_GET(top_index->none_bitmap, old_index)) {
                                // 首槽中的键仍留在右桶，目标空槽在缩小后的右桶区间内。
                                bucket_items[new_index].key = bucket_items[old_index].key;
                                bucket_items[new_index].value = bucket_items[old_index].value;
                                BITMAP_CLEAR(top_index->none_bitmap, new_index);
                                BITMAP_SET(top_index->none_bitmap, old_index);
                            }

                            // 交给左桶的是空槽；两桶占用数 counter 均不变。
                            current_eviction_bucket->begin_index++;
                            current_eviction_bucket->capacity--;
                            previous_eviction_bucket->capacity++;
                            transferred = true;
                        }
                        if (second_lock) second_lock->unlock();
                        first_lock->unlock();
                        if (!transferred) break;
                        temp_target_bucket_for_eviction--;
                        // [修改1 END]
                    }

                    // 驱逐后，再次尝试在原始桶中插入
                    if (checkBucket(bucket_num)) {
                        for(int i = bucket->begin_index; i < bucket->begin_index + bucket->capacity; i++) {
                            if (i < 0 || i >= top_index->item_num) continue;
                            if (BITMAP_GET(top_index->none_bitmap, i)) {
                                top_index->locks_[i].lock();
                                if (i < 0 || i >= top_index->item_num) { top_index->locks_[i].unlock(); continue;}
                                if (BITMAP_GET(top_index->none_bitmap, i)) {
                                    BITMAP_CLEAR(top_index->none_bitmap, i);
                                    bucket_items[i].key = key;
                                    bucket_items[i].value = value;
                                    bucket->counter++;
                                    top_index->locks_[i].unlock();
                                    if (!resize_insert) {
                                        resize_lock_.unlock();
                                    }
                                    return true;
                                }
                                top_index->locks_[i].unlock();
                            }
                        }
                    }
                }
                offset++;
                cur_bucket_num++;
            }

            // 如果所有尝试都失败，考虑 resize
            if (offset == SEARCHRANGE || cur_bucket_num == top_index->bucket_num) {
                if (!resize_insert) {
                    resize_lock_.unlock();   // 在调用 resize 之前释放锁
                    bool resized_successfully = resize(); // resize 函数内部会管理自己的锁
                    resize_lock_.lock();     // resize 后重新获取锁以继续插入操作

                    if (resized_successfully) {
                        goto retry; // 成功 resize 后重试插入（此时 resize_lock_ 已持有）
                    } else {
                        // Resize 失败
                        // resize_lock_ 已在上面获取，这里需要释放因为它将返回 false
                        resize_lock_.unlock();
                        return false;
                    }
                } else {
                    // 在 resize 过程中（resize_insert 为 true），但仍然无法插入
                    // 这是一个异常情况，resize_lock_ 由调用 resize 的函数持有，此处不应释放
                    std::cerr << "Critical error: Cannot insert during resize (all attempts failed after eviction logic)!" << std::endl;
                    return false;
                }
            }

            // 如果没有插入成功，并且不是 resize 过程中的插入，则释放锁
            if (!resize_insert) {
                resize_lock_.unlock();
            }
            return false; // 所有尝试均失败
        }

        bool remove(const Key key) {
            resize_lock_.lock(); // 获取 resize 锁

            if (!top_index || !bucket_items) {
                resize_lock_.unlock();
                // std::cerr << "Error in remove: Hash table not properly initialized." << std::endl;
                return false;
            }

            // 使用 std::hash (hash_func) 保持一致性，原代码此处为 murmurHash
            int bucket_num = hash_func(key) % top_index->bucket_num;
            if (bucket_num < 0 || bucket_num >= top_index->bucket_num) {
                 resize_lock_.unlock();
                 return false; // 无效桶号
            }
            struct bucketInfo *bucket = top_index->buckets + bucket_num;
            int item_index = bucket->begin_index;
            bool removed = false;

            if (item_index < 0 || item_index + bucket->capacity > top_index->item_num) {
                 resize_lock_.unlock();
                 return false; // 桶的索引范围无效
            }

            for (int i = item_index; i < item_index + bucket->capacity; i++) {
                if (i < 0 || i >= top_index->item_num) { // 范围检查
                     std::cerr << "Critical error: Item index out of bounds in remove: " << i
                               << ", item_num: " << top_index->item_num << std::endl;
                    continue;
                }
                if (!BITMAP_GET(top_index->none_bitmap, i)) {
                    top_index->locks_[i].lock(); // 获取元素锁
                     if (i < 0 || i >= top_index->item_num) { // 再次检查
                        top_index->locks_[i].unlock();
                        continue;
                    }
                    if (!BITMAP_GET(top_index->none_bitmap, i) && bucket_items[i].key == key) {
                        BITMAP_SET(top_index->none_bitmap, i); // 标记为空闲
                        bucket->counter--; // 桶内元素计数器减少
                        removed = true;
                        top_index->locks_[i].unlock(); // 释放元素锁
                        break; // 找到并删除，跳出循环
                    }
                    top_index->locks_[i].unlock(); // 释放元素锁
                }
            }

            resize_lock_.unlock(); // 释放 resize 锁
            return removed;
        }

        bool resize() {
            resize_lock_.lock(); // 获取 resize 锁，阻止其他操作

            struct topIndex *old_top_index = top_index;
            struct bucketItem *old_bucket_items = bucket_items;
            SpinLock *old_locks = old_top_index->locks_; // 保存旧锁数组的引用

            // 创建新的哈希表结构
            struct topIndex *new_top_index = topIndex_allocator(1);
            if (!new_top_index) {
                resize_lock_.unlock();
                std::cerr << "Critical error: Failed to allocate new topIndex during resize." << std::endl;
                return false;
            }
            new_top_index->bucket_num = old_top_index->bucket_num * 2;
            new_top_index->item_num = old_top_index->item_num * 2;
            if (new_top_index->item_num == 0 && old_top_index->item_num > 0) { // 溢出或极端情况
                 new_top_index->item_num = old_top_index->item_num; // 保持原样或选择一个大的默认值
            }
            if (new_top_index->bucket_num == 0 && old_top_index->bucket_num > 0) {
                 new_top_index->bucket_num = old_top_index->bucket_num;
            }

            new_top_index->f_seed = old_top_index->f_seed; // 通常保持相同的哈希种子
            new_top_index->buckets = bucketInfo_allocator(new_top_index->bucket_num);
            struct bucketItem *new_bucket_items = bucketItem_allocator(new_top_index->item_num);
            new_top_index->none_bitmap = bitmap_allocator(BITMAP_SIZE(new_top_index->item_num));

            if (!new_top_index->buckets || !new_bucket_items || !new_top_index->none_bitmap) {
                // 清理已分配的新资源
                if (new_top_index->buckets) bucketInfo_deallocator(new_top_index->buckets, new_top_index->bucket_num);
                if (new_bucket_items) bucketItem_deallocator(new_bucket_items, new_top_index->item_num);
                if (new_top_index->none_bitmap) bitmap_deallocator(new_top_index->none_bitmap, BITMAP_SIZE(new_top_index->item_num));
                topIndex_deallocator(new_top_index,1);
                // 恢复旧的指针，尽管这可能不是最佳策略，但至少避免了悬挂指针
                top_index = old_top_index;
                bucket_items = old_bucket_items;
                resize_lock_.unlock();
                std::cerr << "Critical error: Failed to allocate new structures during resize." << std::endl;
                return false;
            }

            memset(new_top_index->none_bitmap, 0xff, sizeof(bitmap_t) * BITMAP_SIZE(new_top_index->item_num));
            new_top_index->locks_ = new SpinLock[new_top_index->item_num]; // 新的锁数组

            // 初始化新桶
            for (int i = 0; i < new_top_index->bucket_num; i++) {
                new_top_index->buckets[i].begin_index = i * SHAREBUCKETSIZE; // 注意 SHAREBUCKETSIZE 是否需要调整
                new_top_index->buckets[i].capacity = SHAREBUCKETSIZE;
                new_top_index->buckets[i].counter = 0;
            }

            // 将类成员指向新的结构，以便 direct_insert_during_resize 使用
            top_index = new_top_index;
            bucket_items = new_bucket_items;

            // 将旧表中的所有元素重新插入到新表中
            // direct_insert_during_resize 是在 resize_lock_ 持有期间调用的，所以它内部不需要锁
            for (int i = 0; i < old_top_index->item_num; i++) {
                if (!BITMAP_GET(old_top_index->none_bitmap, i)) {
                    // 确保旧索引有效
                    if (i < old_top_index->item_num) {
                         // [修改2 BEGIN] 迁移必须逐条成功，失败时保留完整旧表。
                         if (!direct_insert_during_resize(old_bucket_items[i].key, old_bucket_items[i].value)) {
                             top_index = old_top_index;
                             bucket_items = old_bucket_items;
                             bucketItem_deallocator(new_bucket_items, new_top_index->item_num);
                             bucketInfo_deallocator(new_top_index->buckets, new_top_index->bucket_num);
                             bitmap_deallocator(new_top_index->none_bitmap, BITMAP_SIZE(new_top_index->item_num));
                             delete[] new_top_index->locks_;
                             topIndex_deallocator(new_top_index, 1);
                             resize_lock_.unlock();
                             std::cerr << "Error: Resize migration failed; original table retained." << std::endl;
                             return false;
                         }
                         // [修改2 END]
                    }
                }
            }

            // 清理旧的数据结构
            bucketItem_deallocator(old_bucket_items, old_top_index->item_num);
            bucketInfo_deallocator(old_top_index->buckets, old_top_index->bucket_num);
            bitmap_deallocator(old_top_index->none_bitmap, BITMAP_SIZE(old_top_index->item_num));
            delete[] old_locks; // 删除旧的锁数组
            topIndex_deallocator(old_top_index, 1); // 释放旧的顶层索引结构

            std::cout << "resize done!" << std::endl;
            resize_lock_.unlock(); // 释放 resize 锁
            return true;
        }

        // [修改3 BEGIN] 扩容迁移复用本桶插入和修正后的右借位，禁止直接错放到其他桶。
        bool direct_insert_during_resize(const Key& key, const Value& value) {
            // 外部已持有 resize_lock_；true 保留原有的不重入全局锁、不递归扩容语义。
            return insert(key, value, true);
        }
        // [修改3 END]

        // 检查桶是否还有空间 (必须在持有 resize_lock_ 时调用)
        bool checkBucket(int bucket_idx) { // 参数名改为 bucket_idx 以避免与成员变量混淆
            // 确保 bucket_idx 在有效范围内
            if (!top_index || bucket_idx < 0 || bucket_idx >= top_index->bucket_num) {
                std::cerr << "Critical error: Invalid bucket number in checkBucket: " << bucket_idx << std::endl;
                return false; // 或者抛出异常
            }

            bucketInfo *bucket = top_index->buckets + bucket_idx;

            // 确保桶的容量和计数器有效
            if (bucket->capacity <= 0 || bucket->counter < 0 || bucket->counter > bucket->capacity) {
                 // 允许 counter == capacity (桶满)
                 if (bucket->counter > bucket->capacity && bucket->capacity > 0) { // counter 不应大于 capacity
                    std::cerr << "Critical error: Invalid bucket state: capacity=" << bucket->capacity
                             << ", counter=" << bucket->counter << " for bucket " << bucket_idx << std::endl;
                    return false; // 或者尝试修复，或者标记错误
                 }
            }
            return bucket->counter < bucket->capacity;
        }

        long long getBucketNumber() {
            // resize_lock_.lock(); // 如果需要严格保证读取时的一致性，可以加锁，但通常用于调试或状态获取，可能允许轻微的陈旧数据
            if (!top_index) return 0;
            long long num = top_index->bucket_num;
            // resize_lock_.unlock();
            return num;
        }

        long long getItemNumber() {
            // resize_lock_.lock();
            if (!top_index) return 0;
            long long num = top_index->item_num;
            // resize_lock_.unlock();
            return num;
        }

        long long memoryUsage() {
            resize_lock_.lock(); // 保证在计算内存时数据结构稳定
            if (!top_index) {
                resize_lock_.unlock();
                return sizeof(*this);
            }
            long long index_size = 0;
            index_size += sizeof(*this); // sharehash 对象本身的大小
            index_size += top_index->bucket_num * sizeof(bucketInfo);
            index_size += top_index->item_num * sizeof(bucketItem);
            index_size += top_index->item_num * sizeof(SpinLock); // 每个 item 一个锁
            index_size += sizeof(bitmap_t) * BITMAP_SIZE(top_index->item_num); // 位图大小
            index_size += sizeof(topIndex); // topIndex 结构本身的大小

            resize_lock_.unlock();
            return index_size;
        }
    };

}  // namespace ShareHash

#endif //SHAREHASH_SHAREHASH_H
