#include "instrumented_pagepool.h"
#include <random>
#include <vector>
#include <iostream>
int main() {
 std::cout<<"n,home_buckets,pages,samples,failures,slots_per_get,occupied_per_get,directory_hops_per_get\n";
 for (int n : {100000,1000000,5000000}) {
  ShareHash::sharehash_subtable<uint64_t,uint64_t> index;
  std::mt19937_64 gen(20261025);
  std::uniform_int_distribution<int64_t> dis(INT64_MIN,INT64_MAX);
  std::vector<uint64_t> keys; keys.reserve(n);
  int failures=0;
  for(int i=0;i<n;++i) {uint64_t k=dis(gen); keys.push_back(k); if(!index.insert(k,k)) ++failures;}
  ShareHash::diagnostic_slots=ShareHash::diagnostic_occupied=ShareHash::diagnostic_hops=0;
  for(int i=0;i<1000;++i) {auto k=keys[(uint64_t(i)*4999+12345)%n]; uint64_t v=0; if(!index.get(k,v)||v!=k) ++failures;}
  size_t buckets=0;
  for(size_t i=0;i<index.getSubtableCount();++i) buckets+=index.getSubtableHomeBucketNumber(i);
  std::cout<<n<<','<<buckets<<','<<index.getPageCount()<<",1000,"<<failures<<','<<ShareHash::diagnostic_slots/1000.0<<','<<ShareHash::diagnostic_occupied/1000.0<<','<<ShareHash::diagnostic_hops/1000.0<<'\n';
 }
}
