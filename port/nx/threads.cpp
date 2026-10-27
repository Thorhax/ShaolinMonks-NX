// Where threads run on the Switch.
//
// The Xbox has one CPU, and the game's threads were written for one: the kernel time slices them, and x86 orders
// their memory accesses strongly. Every guest thread is pinned to one core here at priority 59, the priority
// Horizon time slices on the application cores (it rotates them every 10 ms), so they share that core the way they
// shared the console's CPU, and never run at once on the ARM cores' weaker memory ordering. The renderer and the
// host's own threads use the other cores.
#include <switch.h>

#include "core/log.h"
#include "nx/platform.h"
#include "port_compat.h"

namespace port {

void BeforeGuestThread(pthread_attr_t*) {}

// Horizon keeps no names for a process's own threads: only the perf lines use them
void NameThread(const char* name) { nx::perf::NameThread(name); }

void OnGuestThread() {
    const auto& t = nx::Threading();
    if (t.guest_core >= 0) {
        Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, t.guest_core, 1u << t.guest_core);
        if (R_FAILED(rc)) LOG_FIRST(1, "threads: guest core %d refused (%X)", t.guest_core, rc);
    } else {
        svcSetThreadCoreMask(CUR_THREAD_HANDLE, -1, 0x7);  // any application core
    }
    Result rc = svcSetThreadPriority(CUR_THREAD_HANDLE, t.guest_priority);
    if (R_FAILED(rc)) LOG_FIRST(1, "threads: guest priority %d refused (%X)", t.guest_priority, rc);
}

void OnRenderThread() {
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, nx::Threading().render_core, 1u << nx::Threading().render_core);
    nx::perf::NameThread("render");
}

}  // namespace port
