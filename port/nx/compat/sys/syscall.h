#pragma once
// Upstream's pipeline precompile workers lower their own priority through Linux calls newlib on Horizon lacks. The
// workers only run for a renderer that starts them, which this port's does not: here the calls do nothing.
#include <sys/types.h>

#define SYS_gettid 0
#ifndef PRIO_PROCESS
#define PRIO_PROCESS 0
#endif
inline long syscall(long, ...) { return 0; }
inline int setpriority(int, id_t, int) { return 0; }
