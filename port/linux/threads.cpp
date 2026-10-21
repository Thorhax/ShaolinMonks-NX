#include "port_compat.h"

namespace port {
void BeforeGuestThread(pthread_attr_t*) {}
void OnGuestThread() {}
void NameThread(const char* name) { pthread_setname_np(pthread_self(), name); }
void OnRenderThread() { pthread_setname_np(pthread_self(), "render"); }
}  // namespace port
