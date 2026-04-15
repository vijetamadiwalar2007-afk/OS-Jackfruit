#include <stdio.h>
#include <stdlib.h>
#include <time.h>

int main(int argc, char *argv[]) {

    long iterations = argc > 1 ? atol(argv[1]) : 1000000000L;

    printf("cpu_burn: starting %ld iterations\n", iterations);
    fflush(stdout);

    struct timespec start, end;
    clock_gettime(CLOCK_MONOTONIC, &start);

    volatile long x = 0;

    for (long i = 0; i < iterations; i++) {
        x = x * 1000003 + 1;
    }

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed = (end.tv_sec - start.tv_sec) +
                     (end.tv_nsec - start.tv_nsec) / 1e9;

    printf("cpu_burn: done in %.2f seconds\n", elapsed);
    fflush(stdout);

    return 0;
}
