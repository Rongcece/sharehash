// Focused, identical workload for PagePool B and the ordinary subtable version.
// c++ -std=gnu++14 -pg -g -O1 -fopenmp benchmark/pagepoolB_benchmark.cpp -o /tmp/pagepoolB_benchmark
// /tmp/pagepoolB_benchmark pagepoolB 5000000 20261025
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#define sharehash_subtable pagepoolB_index
#include "../include/ShareHash_pagepoolB.h"
#undef sharehash_subtable
#define sharehash_subtable ordinary_subtable_index
#include "../include/ShareHash_subtable.h"
#undef sharehash_subtable
#include "ResultWriter.h"

template<class T> int run(const std::string& type, std::size_t n, std::uint64_t seed) {
    auto before_init=benchmark_results::sample_memory();
    T table;
    auto after_init=benchmark_results::sample_memory();
    std::mt19937_64 gen(seed);
    std::uniform_int_distribution<std::int64_t> distribution(INT64_MIN,INT64_MAX);
    std::vector<std::pair<std::uint64_t,std::uint64_t>> input;
    input.reserve(n);
    for(std::size_t i=0;i<n;++i){auto key=static_cast<std::uint64_t>(distribution(gen));input.push_back({key,key});}
    auto before_insert=benchmark_results::sample_memory();
    std::size_t inserted=0,hits=0,wrong=0;
    auto start=std::chrono::steady_clock::now();
    for(auto pair:input)inserted+=table.insert(pair.first,pair.second);
    const double insert_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    auto after_insert=benchmark_results::sample_memory();
    start=std::chrono::steady_clock::now();
    for(auto pair:input){std::uint64_t value=0; bool found=table.get(pair.first,value);hits+=found;wrong+=found&&value!=pair.second;}
    const double read_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    const long long heap=(after_init.heap_bytes-before_init.heap_bytes)+(after_insert.heap_bytes-before_insert.heap_bytes);
    const long long locks=(table.getItemNumber()+table.getSubtableCount()+(type=="pagepoolB"))*sizeof(ShareHash::SpinLock);
    std::size_t index_buckets=0,splits=0;
    for(std::size_t i=0;i<table.getSubtableCount();++i){index_buckets+=table.getSubtableBucketNumber(i);splits+=table.getSubtableResizeCount(i);}
    std::cout.precision(17);
    std::cout<<type<<','<<n<<','<<seed<<','<<inserted<<','<<hits<<','<<wrong<<','
             <<insert_seconds<<','<<read_seconds<<','<<n/insert_seconds<<','<<n/read_seconds<<','
             <<table.memoryUsage()<<','<<heap<<','<<locks<<','<<double(heap-locks)/n<<','
             <<index_buckets<<','<<splits<<'\n';
    return inserted==n&&hits==n&&!wrong?0:1;
}
int main(int argc,char** argv){
    if(argc!=4){std::cerr<<"usage: binary pagepoolB|subtable N SEED\n";return 2;}
    const std::string type=argv[1];const auto n=std::stoull(argv[2]);const auto seed=std::stoull(argv[3]);
    if(!n)return 2;
    if(type=="pagepoolB")return run<ShareHash::pagepoolB_index<std::uint64_t,std::uint64_t>>(type,n,seed);
    if(type=="subtable")return run<ShareHash::ordinary_subtable_index<std::uint64_t,std::uint64_t>>(type,n,seed);
    return 2;
}
