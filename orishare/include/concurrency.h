//
// Created by admin on 2024/1/26.
//

#ifndef SHAREHASH_CONCURRENCY_H
#define SHAREHASH_CONCURRENCY_H

#include <atomic>
#include <boost/thread.hpp>

namespace ShareHash{
    class SpinLock{
    public:
        SpinLock(){
            lock_.store(false);
        }
        ~SpinLock(){
            lock_.store(false);
        }

        void init(){
            lock_.store(false);
        }

        void lock(){
            // 添加自旋等待优化，减少总线流量
            while(lock_.exchange(true, std::memory_order_acquire)){
                // 在循环中添加短暂等待，减少CPU使用
                while(lock_.load(std::memory_order_relaxed)) {
                    #if defined(__x86_64__) || defined(__i386__)
                    __asm__ __volatile__ ("pause" ::: "memory");
                    #elif defined(__aarch64__)
                    __asm__ __volatile__ ("yield" ::: "memory");
                    #endif
                }
            }
        }

        bool try_lock() {
            return !lock_.exchange(true, std::memory_order_acquire);
        }

        void unlock(){
            lock_.store(false, std::memory_order_release);
        }

        bool test(){
            return lock_.load(std::memory_order_acquire);
        }

        void wait(){
            while(lock_.load(std::memory_order_acquire)){
                #if defined(__x86_64__) || defined(__i386__)
                __asm__ __volatile__ ("pause" ::: "memory");
                #elif defined(__aarch64__)
                __asm__ __volatile__ ("yield" ::: "memory");
                #endif
            }
        }
    private:
        std::atomic_bool lock_;


    };

}

#endif //SHAREHASH_CONCURRENCY_H
