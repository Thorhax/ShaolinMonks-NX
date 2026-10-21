// POSIX calls the runtime makes that newlib on Horizon lacks
#include <errno.h>
#include <switch.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <mutex>
#include <vector>

// libnx's pthread handle (runtime/newlib.c): the Thread comes first; it is allocated with __libnx_alloc
struct __pthread_t {
    Thread thr;
    void* rc;
};
extern "C" void __libnx_free(void* p);

namespace {
std::mutex g_detached_lock;
std::vector<__pthread_t*> g_detached;

bool Exited(__pthread_t* t) { return R_SUCCEEDED(svcWaitSynchronizationSingle(t->thr.handle, 0)); }
}  // namespace

extern "C" {

// a positioned read: seek and read as one step, so threads reading the disc image at once do not interleave
ssize_t pread(int fd, void* buf, size_t count, off_t offset) {
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    const off_t was = lseek(fd, 0, SEEK_CUR);
    if (lseek(fd, offset, SEEK_SET) < 0) return -1;
    const ssize_t got = read(fd, buf, count);
    lseek(fd, was, SEEK_SET);
    return got;
}

// relaunching into another executable: Shaolin Monks' bonus games are not supported on this host
int execl(const char*, const char*, ...) {
    errno = ENOSYS;
    return -1;
}

// libnx leaves detaching out, so pthread_detach and std::thread::detach fail with ENOSYS. A detached thread is
// kept here until it has ended, then closed (its stack freed) the next time a thread is detached.
int __syscall_thread_detach(__pthread_t* thread) {
    std::lock_guard<std::mutex> guard(g_detached_lock);
    auto ended = std::partition(g_detached.begin(), g_detached.end(), [](__pthread_t* t) { return !Exited(t); });
    for (auto it = ended; it != g_detached.end(); ++it) {
        threadClose(&(*it)->thr);
        __libnx_free(*it);
    }
    g_detached.erase(ended, g_detached.end());
    g_detached.push_back(thread);
    return 0;
}

}  // extern "C"
