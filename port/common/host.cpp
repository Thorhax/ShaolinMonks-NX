// The options upstream reads from the command line; this host runs with the defaults
#include "host.h"

#include <atomic>

#include "port_compat.h"

namespace openshao {
Options options;

namespace {
std::atomic<bool> g_exit{false};
}  // namespace
// the main menu's Exit row: the host's main loop ends the run (on the Switch, back to the Homebrew Menu)
bool RequestExit() {
    g_exit = true;
    return true;
}
}  // namespace openshao

bool port::ExitRequested() { return openshao::g_exit; }
