#include "parallel.h"

#include <atomic>

#include "c8/task-queue.h"
#include "c8/thread-pool.h"

struct Parallel {
  c8::ThreadPool threadPool;
  c8::TaskQueue taskQueue;
  std::atomic<int> firstError{0};

  explicit Parallel(int numThreads) : threadPool(numThreads) {}
};

extern "C" {

  Parallel *initParallel(int numThreads) {
    return new Parallel(numThreads);
  }

  void freeParallel(Parallel *parallel) {
    delete parallel;
  }

  void scheduleParallel(Parallel *parallel, int (*fn)(void *), void *data) {
    parallel->taskQueue.addTask([=] {
      int res = fn(data);
      int expected = 0;
      parallel->firstError.compare_exchange_strong(
        expected,
        res,
        std::memory_order_relaxed);
    });
  }

  int runParallel(Parallel *parallel) {
    parallel->taskQueue.executeWithThreadPool(&parallel->threadPool);
    return parallel->firstError;
  }
}
