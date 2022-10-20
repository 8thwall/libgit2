#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Parallel Parallel;

Parallel *initParallel(int numThreads);

void freeParallel(Parallel *);

void scheduleParallel(Parallel*, int (*fn)(void *), void *data);

int runParallel(Parallel *);

#ifdef __cplusplus
}  // Closes the extern "C"
#endif
