// learning/ch14/thread_pool.cpp
//
// Read in this order:
//   1. worker               what every pool thread does, forever
//   2. thread_pool_init     create the lock, the doorbell and the threads
//   3. thread_pool_queue    how a job is handed over

#include <assert.h>

#include "thread_pool.h"

// ---------------------------------------------------------------
// 1. The worker: this function runs in EACH pool thread.
// ---------------------------------------------------------------
//
// The pattern is the same in every program that uses a condition
// variable:
//
//     lock
//     while (nothing to do)  wait
//     take the job
//     unlock
//     do the job            <-- outside the lock!
//
// pthread_cond_wait does THREE things as one atomic step:
//     releases the mutex, puts the thread to sleep, and when it is
//     woken up takes the mutex again before returning.
// Releasing the mutex while asleep is essential: otherwise the
// producer could never lock it to add a job.
static void *worker(void *arg) {
    ThreadPool *tp = (ThreadPool *)arg;
    while (true) {
        pthread_mutex_lock(&tp->mu);          // only we touch the queue now

        // Wait for the condition: a non-empty queue.
        //
        // This MUST be a `while`, not an `if`. Being woken up does not
        // guarantee a job is still there:
        //   1. the producer signals and unlocks
        //   2. ANOTHER worker grabs the mutex first and takes the job
        //   3. we wake up and get the mutex, but the queue is empty again
        // So after every wake-up we check again, and go back to sleep if needed.
        while (tp->queue.empty()) {
            pthread_cond_wait(&tp->not_empty, &tp->mu);
        }

        // We hold the mutex and the queue is not empty: take a job.
        Work w = tp->queue.front();
        tp->queue.pop_front();
        pthread_mutex_unlock(&tp->mu);        // let others use the queue right away

        // Do the job. This is outside the lock on purpose: the job may
        // be slow, and other workers must be free to take other jobs.
        w.f(w.arg);
    }
    return nullptr;
}

// ---------------------------------------------------------------
// 2. Setting up the pool
// ---------------------------------------------------------------
void thread_pool_init(ThreadPool *tp, size_t num_threads) {
    assert(num_threads > 0);

    int rv = pthread_mutex_init(&tp->mu, nullptr);
    assert(rv == 0);
    rv = pthread_cond_init(&tp->not_empty, nullptr);
    assert(rv == 0);

    tp->threads.resize(num_threads);
    for (size_t i = 0; i < num_threads; ++i) {
        // start a thread that runs worker(tp)
        rv = pthread_create(&tp->threads[i], nullptr, &worker, tp);
        assert(rv == 0);
    }
    (void)rv;   // (only used by the asserts)
}

// ---------------------------------------------------------------
// 3. Handing over a job (the "producer" side)
// ---------------------------------------------------------------
void thread_pool_queue(ThreadPool *tp, void (*f)(void *), void *arg) {
    Work w;
    w.f = f;
    w.arg = arg;

    pthread_mutex_lock(&tp->mu);              // the queue is shared: lock it
    tp->queue.push_back(w);
    pthread_cond_signal(&tp->not_empty);      // ring the doorbell: wake ONE sleeping worker
    pthread_mutex_unlock(&tp->mu);
}
