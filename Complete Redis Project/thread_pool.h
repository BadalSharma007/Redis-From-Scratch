// learning/ch14/thread_pool.h
//
// A thread pool: a fixed group of worker threads that run jobs taken
// from a shared queue. Used to move slow work (freeing a huge sorted
// set) off the main thread so the event loop never stalls.
//
// A JOB is "call f(arg)": a plain function pointer plus one argument.
// (The same callback-plus-void* pattern as h_scan in chapter 9.)
//
// The queue is shared by several threads, so it is protected by two
// pthread tools:
//   - a MUTEX:              only one thread may touch the queue at a time
//   - a CONDITION VARIABLE: idle workers SLEEP until the queue has work,
//                           instead of looping and wasting CPU
//
// Note: this pool has no shutdown. The workers run until the process
// exits, which is all our server needs.

#pragma once

#include <pthread.h>
#include <stddef.h>

#include <deque>
#include <vector>

struct Work {
    void (*f)(void *) = nullptr;   // the function to run
    void *arg = nullptr;           // the argument to pass to it
};

struct ThreadPool {
    std::vector<pthread_t> threads;   // the worker threads
    std::deque<Work> queue;           // jobs waiting to be run (protected by `mu`)
    pthread_mutex_t mu;               // the lock that protects `queue`
    pthread_cond_t not_empty;         // "the queue just got a job": wakes a sleeping worker
};

// Initialize the pool and START num_threads worker threads.
void thread_pool_init(ThreadPool *tp, size_t num_threads);

// Add a job: some worker will eventually call f(arg).
// Safe to call from any thread.
void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg);
