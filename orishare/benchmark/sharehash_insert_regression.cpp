/*
 * Regression coverage added for the ShareHash insertion and resize fixes.
 * This new file exercises only existing public APIs; it does not change the
 * benchmark, hashmap layout, search behavior, or duplicate-key semantics.
 * The small crafted cases check bucket ownership indirectly through get()
 * and exact values after every operation. No private-state access is used.
 *
 * Build from the project root (C++14, no test framework required):
 *   c++ -std=c++14 -O2 -pthread benchmark/sharehash_insert_regression.cpp \
 *       -o /tmp/sharehash_insert_regression
 * Run all cases, or pass one case name printed by the full run:
 *   /tmp/sharehash_insert_regression
 *   /tmp/sharehash_insert_regression resize_home_bucket_full
 */

#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>

#include "../include/ShareHash.h"

// Test-only hash mode switch: normal mode keeps five keys dispersed in the old
// table, while collapsed mode sends all migration keys to the new last bucket.
// This deliberately violates hash stability only to force the resize-failure
// branch. Restore normal mode before checking the unchanged original table.
// No production key type or production hash function is modified by this test.
struct RollbackKey {
    std::uint64_t id;
    static bool collapse;
    static std::uint64_t collapsed_hash;

    bool operator==(const RollbackKey& other) const {
        return id == other.id;
    }
};

bool RollbackKey::collapse = false;
std::uint64_t RollbackKey::collapsed_hash = 0;

namespace std {

template<>
struct hash<RollbackKey> {
    std::size_t operator()(const RollbackKey& key) const noexcept {
        return static_cast<std::size_t>(RollbackKey::collapse
                                       ? RollbackKey::collapsed_hash : key.id);
    }
};

}  // namespace std

namespace {

using Key = std::uint64_t;
using Value = std::uint64_t;
using Table = ShareHash::sharehash<Key, Value>;
using Expected = std::map<Key, Value>;

// These checks deliberately remain active when the file is built with NDEBUG.
void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

Value value_for(Key key) {
    return (key * UINT64_C(0x9e3779b97f4a7c15)) ^ UINT64_C(0xd1b54a32d192ed03);
}

// The project uses std::hash<uint64_t>. Verify the crafted bucket mapping on
// the current implementation rather than silently assuming integer hashing.
Key bucket_key(Key bucket, Key ordinal, Key bucket_count) {
    const Key key = bucket + ordinal * bucket_count;
    require(std::hash<Key>{}(key) % bucket_count == bucket,
            "crafted integer key does not map to the intended bucket");
    return key;
}

void verify_all(Table& table, const Expected& expected) {
    for (const auto& entry : expected) {
        Value actual = ~entry.second;
        require(table.get(entry.first, actual),
                "get missed inserted key " + std::to_string(entry.first));
        require(actual == entry.second,
                "get returned the wrong value for key " + std::to_string(entry.first));
    }
}

void insert_value(Table& table, Expected& expected, Key key, Value value,
                  bool verify_immediately = true) {
    require(expected.count(key) == 0, "test attempted to insert a duplicate live key");
    require(table.insert(key, value), "insert failed for key " + std::to_string(key));
    expected.emplace(key, value);
    if (verify_immediately) {
        verify_all(table, expected);
    }
}

void insert_key(Table& table, Expected& expected, Key key,
                bool verify_immediately = true) {
    insert_value(table, expected, key, value_for(key), verify_immediately);
}

// The right donor is empty, so its first physical slot can be donated directly.
void right_donor_first_slot_empty() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    for (Key ordinal = 0; ordinal < 5; ++ordinal) {
        insert_key(table, expected, bucket_key(0, ordinal, buckets));
    }
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "borrowing one free slot unexpectedly resized the table");
}

// This exact eight-key sequence occupied the donor's first three slots before
// borrowing. Moving its boundary without preserving those keys caused misses.
void right_donor_first_slot_occupied() {
    Table table;
    Expected expected;
    require(table.getBucketNumber() == 5000,
            "the literal eight-key regression expects the current 5000-bucket default");
    const Key keys[] = {1, 5001, 10001, 0, 5000, 10000, 15000, 20000};
    for (Key key : keys) {
        insert_key(table, expected, key);
    }
    require(table.getBucketNumber() == 5000,
            "the eight-key borrow regression unexpectedly resized the table");
}

// Buckets 0, 1, and 2 are full. Bucket 3 has an occupied first slot and room
// later in its range, forcing the free slot through three successive boundaries.
void borrow_through_multiple_buckets() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    for (Key bucket = 0; bucket < 3; ++bucket) {
        for (Key ordinal = 0; ordinal < 4; ++ordinal) {
            insert_key(table, expected, bucket_key(bucket, ordinal, buckets));
        }
    }
    insert_key(table, expected, bucket_key(3, 0, buckets));
    insert_key(table, expected, bucket_key(3, 1, buckets));
    insert_key(table, expected, bucket_key(0, 4, buckets));
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "a multi-bucket borrow unnecessarily resized the table");
}

// Eight keys in bucket 0 consume its four initial slots and all four slots of
// empty bucket 1. The ninth must pass a free slot back through that zero-capacity
// bucket. Then inserting into bucket 1 must restore its capacity by borrowing.
void borrow_through_zero_capacity_bucket() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    for (Key ordinal = 0; ordinal < 9; ++ordinal) {
        insert_key(table, expected, bucket_key(0, ordinal, buckets));
    }
    insert_key(table, expected, bucket_key(1, 0, buckets));
    insert_key(table, expected, bucket_key(0, 9, buckets));
    insert_key(table, expected, bucket_key(1, 1, buckets));
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "a zero-capacity intermediate bucket unnecessarily caused resize");
}

// Remove keys from both sides after a borrow, verify absence, and reinsert only
// those now-absent keys with new values. This does not test duplicate-key updates.
void remove_then_reinsert() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    const Key keys[] = {1, buckets + 1, 2 * buckets + 1,
                        0, buckets, 2 * buckets, 3 * buckets, 4 * buckets};
    for (Key key : keys) {
        insert_key(table, expected, key);
    }
    const Key removed[] = {1, 2 * buckets};
    for (Key key : removed) {
        require(table.remove(key), "remove failed for key " + std::to_string(key));
        expected.erase(key);
        Value actual = 0;
        require(!table.get(key, actual), "removed key is still reachable");
        verify_all(table, expected);
    }
    for (Key key : removed) {
        insert_value(table, expected, key, value_for(key) ^ UINT64_C(0xffff0000ffff0000));
    }
    insert_key(table, expected, bucket_key(0, 5, buckets));
    insert_key(table, expected, bucket_key(1, 3, buckets));
}

// Remove the donor's first key before a borrow so the implementation must also
// handle an empty boundary slot when occupied keys remain later in that bucket.
void right_donor_first_slot_removed() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    for (Key ordinal = 0; ordinal < 3; ++ordinal) {
        insert_key(table, expected, bucket_key(1, ordinal, buckets));
    }
    require(table.remove(1), "could not create the donor's leading hole");
    expected.erase(1);
    for (Key ordinal = 0; ordinal < 5; ++ordinal) {
        insert_key(table, expected, bucket_key(0, ordinal, buckets));
    }
    insert_value(table, expected, 1, value_for(1) ^ UINT64_C(0x123456789abcdef0));
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "borrowing around a deleted boundary slot unnecessarily resized the table");
}

// Six unique keys share the same home bucket both before and after doubling.
// The public resize() therefore migrates more than four keys into the new home
// bucket, exercising the former migration-only overflow placement mismatch.
// Every key must remain visible to the ordinary single-home-bucket get().
void resize_home_bucket_full() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    const long long slots = table.getItemNumber();
    for (Key ordinal = 0; ordinal < 6; ++ordinal) {
        const Key key = bucket_key(7, ordinal, 2 * buckets);
        require(std::hash<Key>{}(key) % buckets == 7,
                "migration key has an unexpected original home bucket");
        insert_key(table, expected, key);
    }
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "preparing migration collisions unexpectedly resized the table");
    require(table.resize(), "explicit public resize failed");
    require(table.getBucketNumber() == static_cast<long long>(2 * buckets),
            "resize did not double the bucket count");
    require(table.getItemNumber() == 2 * slots, "resize did not double slot capacity");
    verify_all(table, expected);
}

// Force migration failure only in the candidate table: five entries all map
// to its last four-slot bucket, which cannot borrow past the right edge. After
// resize() returns false, restore the original stable hash mapping and confirm
// both original dimensions and every old value survived rollback unchanged.
void failed_resize_restores_original_table() {
    using RollbackTable = ShareHash::sharehash<RollbackKey, Value>;
    RollbackTable table;
    const long long buckets = table.getBucketNumber();
    const long long slots = table.getItemNumber();
    for (Key id = 0; id < 5; ++id) {
        require(table.insert(RollbackKey{id}, value_for(id)),
                "could not prepare dispersed rollback-test keys");
    }
    RollbackKey::collapsed_hash = static_cast<Key>(2 * buckets - 1);
    RollbackKey::collapse = true;
    const bool resized = table.resize();
    RollbackKey::collapse = false;
    require(!resized, "resize unexpectedly accepted five keys in the last four-slot bucket");
    require(table.getBucketNumber() == buckets,
            "failed resize did not restore the original bucket count");
    require(table.getItemNumber() == slots,
            "failed resize did not restore the original slot capacity");
    for (Key id = 0; id < 5; ++id) {
        Value actual = ~value_for(id);
        require(table.get(RollbackKey{id}, actual),
                "failed resize lost an original key after hash mode was restored");
        require(actual == value_for(id), "failed resize corrupted an original value");
    }
    require(table.insert(RollbackKey{5}, value_for(5)),
            "original table is not writable after failed-resize rollback");
    Value actual = 0;
    require(table.get(RollbackKey{5}, actual) && actual == value_for(5),
            "could not retrieve a key inserted after failed-resize rollback");
}

// Real fixed std::hash<uint64_t> rollback regression, without the test-only
// hash switch above. Fill home bucket 0 with 2000 keys that map to bucket 0
// before and after doubling. It then owns all initial slots from buckets
// 0..499. A temporary key in home 499 borrows a slot from bucket 500; removing
// that key leaves a free slot just inside bucket 0's SEARCHRANGE. Bucket 0 can
// borrow that slot for key 2001 in the original table, but a fresh doubled
// candidate has only the original 2000 slots reachable inside that range.
// Migration must therefore fail cleanly and preserve all original key/value
// pairs and dimensions. Validate complete stages, not every insertion, to
// avoid quadratic test-oracle work for the crafted collision-heavy sequence.
void fixed_hash_failed_resize_restores_original_table() {
    Table table;
    Expected expected;
    const Key buckets = table.getBucketNumber();
    const long long slots = table.getItemNumber();
    require(buckets == 5000 && SEARCHRANGE == 500 && SHAREBUCKETSIZE == 4,
            "fixed-hash rollback construction expects current bucket/search defaults");
    for (Key ordinal = 0; ordinal < 2000; ++ordinal) {
        insert_key(table, expected, bucket_key(0, ordinal, 2 * buckets), false);
    }
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "preparing 2000 colliding keys unexpectedly resized the original table");
    verify_all(table, expected);

    insert_key(table, expected, bucket_key(499, 0, buckets), false);
    require(table.remove(499), "could not remove the temporary bucket-499 key");
    expected.erase(499);
    Value removed_value = 0;
    require(!table.get(499, removed_value), "the temporary bucket-499 key remains reachable");
    insert_key(table, expected, bucket_key(0, 2000, 2 * buckets), false);
    require(expected.size() == 2001, "fixed-hash rollback construction has the wrong key count");
    require(table.getBucketNumber() == static_cast<long long>(buckets)
                && table.getItemNumber() == slots,
            "bridging the old search boundary unexpectedly resized the original table");
    verify_all(table, expected);

    require(!table.resize(), "fixed-hash migration beyond SEARCHRANGE unexpectedly succeeded");
    require(table.getBucketNumber() == static_cast<long long>(buckets),
            "fixed-hash failed resize changed the original bucket count");
    require(table.getItemNumber() == slots,
            "fixed-hash failed resize changed the original slot capacity");
    verify_all(table, expected);
}

// Keep the original right-edge/no-wrap policy: fill the current last bucket to
// cause insertion-triggered doubling, and check every old value after each
// operation. Repeat four times to cover multiple migrations on one table.
void multiple_insertion_triggered_resizes() {
    Table table;
    Expected expected;
    for (Key round = 0; round < 4; ++round) {
        const Key buckets = table.getBucketNumber();
        const long long slots = table.getItemNumber();
        for (Key attempt = 0; attempt < 64
                && table.getBucketNumber() == static_cast<long long>(buckets); ++attempt) {
            const Key key = bucket_key(buckets - 1, 100000 + round * 128 + attempt, buckets);
            if (expected.count(key) == 0) {
                insert_key(table, expected, key);
            }
        }
        require(table.getBucketNumber() == static_cast<long long>(2 * buckets),
                "filling the current last bucket did not cause one doubling");
        require(table.getItemNumber() == 2 * slots,
                "insertion-triggered resize did not double slot capacity");
        verify_all(table, expected);
    }
}

// Fixed seed and unique live keys make this a reproducible correctness test.
// Final verification checks all 100000 returned values, not only the hit count.
void random_100000() {
    Table table;
    Expected expected;
    std::mt19937_64 generator(UINT64_C(20261025));
    while (expected.size() < 100000) {
        const Key key = generator();
        if (expected.count(key) == 0) {
            insert_key(table, expected, key, false);
        }
        if (expected.size() == 10000 || expected.size() == 50000) {
            verify_all(table, expected);
        }
    }
    verify_all(table, expected);
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main(int argc, char** argv) {
    const TestCase tests[] = {
        {"right_donor_first_slot_empty", right_donor_first_slot_empty},
        {"right_donor_first_slot_occupied", right_donor_first_slot_occupied},
        {"borrow_through_multiple_buckets", borrow_through_multiple_buckets},
        {"borrow_through_zero_capacity_bucket", borrow_through_zero_capacity_bucket},
        {"remove_then_reinsert", remove_then_reinsert},
        {"right_donor_first_slot_removed", right_donor_first_slot_removed},
        {"resize_home_bucket_full", resize_home_bucket_full},
        {"failed_resize_restores_original_table", failed_resize_restores_original_table},
        {"fixed_hash_failed_resize_restores_original_table", fixed_hash_failed_resize_restores_original_table},
        {"multiple_insertion_triggered_resizes", multiple_insertion_triggered_resizes},
        {"random_100000", random_100000},
    };
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0] << " [case_name]\n";
        return 2;
    }
    std::size_t passed = 0;
    try {
        for (const TestCase& test : tests) {
            if (argc == 2 && std::string(argv[1]) != test.name) {
                continue;
            }
            std::cout << "[ RUN  ] " << test.name << std::endl;
            test.run();
            ++passed;
            std::cout << "[ PASS ] " << test.name << std::endl;
        }
        require(passed != 0, "no test case matched the requested name");
    } catch (const std::exception& error) {
        std::cerr << "[ FAIL ] " << error.what() << '\n';
        return 1;
    }
    std::cout << "Passed " << passed << " ShareHash insertion regression cases.\n";
    return 0;
}
