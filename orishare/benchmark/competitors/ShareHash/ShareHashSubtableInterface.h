#ifndef SHAREHASH_SUBTABLE_INTERFACE_H
#define SHAREHASH_SUBTABLE_INTERFACE_H

#include "ShareHash_subtable.h"
#include "../MapInterface.h"
#include <memory>

// Independent benchmark competitor. Default construction uses the subtable
// implementation's automatic T, load threshold and total initial bucket count.
template<class Key, class Value>
class ShareHashSubtableInterface : public MapInterface<Key, Value> {
public:
    void init() override {
        index = std::make_unique<ShareHash::sharehash_subtable<Key, Value>>();
    }

    void bulk_load(std::pair<Key, Value>* kvPairs, long long dataNumber) override {
        for (long long i = 0; i < dataNumber; ++i)
            index->insert(kvPairs[i].first, kvPairs[i].second);
    }

    bool get(Key key, Value& val) override { return index->get(key, val); }

    bool insert(Key key, Value val) override { return index->insert(key, val); }

    long long memory_consumption() override { return index->memoryUsage(); }

    long long lock_memory_consumption() override {
        // Every slot has a lock; each stable subtable control block also
        // embeds one resize lock. This is logical lock storage, not an RSS
        // measurement or the allocator's rounded allocation size.
        return index ? (index->getItemNumber()
            + static_cast<long long>(index->getSubtableCount()))
            * static_cast<long long>(sizeof(ShareHash::SpinLock)) : 0;
    }

private:
    std::unique_ptr<ShareHash::sharehash_subtable<Key, Value>> index;
};

#endif // SHAREHASH_SUBTABLE_INTERFACE_H
