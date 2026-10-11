#ifndef V4X_BENCH_PROBES_H
#define V4X_BENCH_PROBES_H

// Times a 1 ms sleep and the CPU's writes into GPU memory, and logs them (xsdk:SCOPE.md, milestone 19, step 1).
void probes_run(void);

#endif
