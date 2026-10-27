//
// Created by yyx on 24-1-19.
//

#ifndef SHAREHASH_MAPINTERFACE_H
#define SHAREHASH_MAPINTERFACE_H

#include <utility>

template<class Key, class Value>
class MapInterface{
public:
    virtual void bulk_load(std::pair<Key, Value> *kvPairs, long long dataNumber) = 0;

    virtual bool get(Key key, Value &val) = 0;

    virtual bool insert(Key key, Value val) = 0;

    virtual void init() = 0;

    virtual long long memory_consumption() = 0;

    // Separately identified lock storage, used only for explicit memory/BPK
    // adjustments. Zero means no exclusion is supplied by this adapter; it
    // does not assert that the implementation contains no locks.
    virtual long long lock_memory_consumption() { return 0; }

    virtual ~MapInterface() = default;

};

#endif //SHAREHASH_MAPINTERFACE_H
