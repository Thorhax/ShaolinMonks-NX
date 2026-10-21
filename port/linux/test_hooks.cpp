// Test bed only, files the run script drops in the run folder:
//   dump  holding an output name: guest memory written there, every committed 4 KB page as its address (4 bytes)
//         then its contents. Used to find the title's variables.
//   menu  holding button names joined by '+' (UP DOWN LEFT RIGHT A B X Y MINUS PLUS): one press on the quick menu
//   peek  "<address or char0/char1> <length> <output>" (hex numbers): that range of guest memory, quickly
//   watch "<address> [<length>]" or "off": every instruction that touches those pages of guest memory (4 KB if no
//         length) is logged, by its
//         offset in the binary, to watch.txt when turned off (addr2line names the recompiled function)
//   poke  "<address> <value>" (hex numbers): a 32-bit write. An address "*A+B" is the word at A plus B, for
//         variables in the title's heap
//   jump  "1" or "0": player 1 holds jump or lets go, for the moon jump cheat (the pad script's presses are not seen)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <dlfcn.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "cheats.h"
#include "quick_menu.h"

namespace port {
namespace {

uint32_t MenuButtons(const std::string& names) {
    static const std::pair<const char*, uint32_t> kNames[] = {
        {"UP", menu::kUp}, {"DOWN", menu::kDown}, {"LEFT", menu::kLeft}, {"RIGHT", menu::kRight}, {"A", menu::kA},
        {"B", menu::kB}, {"X", menu::kX}, {"Y", menu::kY}, {"MINUS", menu::kMinus}, {"PLUS", menu::kPlus}};
    uint32_t bits = 0;
    size_t at = 0;
    while (at <= names.size()) {
        size_t end = names.find('+', at);
        if (end == std::string::npos) end = names.size();
        const std::string name = names.substr(at, end - at);
        for (const auto& [n, b] : kNames)
            if (name == n) bits |= b;
        at = end + 1;
    }
    return bits;
}

void PressMenu() {
    const auto trigger = app::ExeDir() / "menu";
    std::ifstream in(trigger);
    if (!in) return;
    std::string names;
    std::getline(in, names);
    in.close();
    std::filesystem::remove(trigger);
    const uint32_t bits = MenuButtons(names);
    menu::UpdateInput(bits, bits);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    menu::UpdateInput(0, 0);
    LOGR("test: menu %s", names.c_str());
}

uint32_t Resolve(const std::string& where) {
    if (where.rfind("char", 0) == 0) return kernel::R32(0x5D1CB8 + uint32_t(std::stoi(where.substr(4))) * 0x3238);
    if (where.rfind('*', 0) == 0) {
        const size_t plus = where.find('+');
        const uint32_t at = uint32_t(std::stoul(where.substr(1, plus - 1), nullptr, 16));
        if (!mem::IsCommitted(at)) return 0;
        return kernel::R32(at) + (plus == std::string::npos ? 0 : uint32_t(std::stoul(where.substr(plus + 1), nullptr, 16)));
    }
    return uint32_t(std::stoul(where, nullptr, 16));
}

// the watched page: a fault records the instruction, opens the page and single-steps it; the trap closes it again
uintptr_t g_watch = 0, g_watch_len = 0;
std::atomic<int> g_hits{0};
constexpr int kSlots = 1 << 15;
uintptr_t g_pcs[kSlots];  // open addressing by the instruction's address
struct sigaction g_old_segv, g_old_trap;

void OnSegv(int sig, siginfo_t* info, void* ctx) {
    const uintptr_t at = reinterpret_cast<uintptr_t>(info->si_addr);
    if (!g_watch || at < g_watch || at >= g_watch + g_watch_len) {
        sigaction(SIGSEGV, &g_old_segv, nullptr);
        raise(sig);
        return;
    }
    auto* uc = static_cast<ucontext_t*>(ctx);
    const uintptr_t pc = uintptr_t(uc->uc_mcontext.gregs[REG_RIP]);
    for (uint32_t i = uint32_t(pc * 0x9E3779B1u) % kSlots, k = 0; k < kSlots; i = (i + 1) % kSlots, k++) {
        if (g_pcs[i] == pc) break;
        if (!g_pcs[i]) {
            g_pcs[i] = pc;
            g_hits++;
            break;
        }
    }
    mprotect(reinterpret_cast<void*>(g_watch), g_watch_len, PROT_READ | PROT_WRITE);
    uc->uc_mcontext.gregs[REG_EFL] |= 0x100;
}

void OnTrap(int, siginfo_t*, void* ctx) {
    auto* uc = static_cast<ucontext_t*>(ctx);
    uc->uc_mcontext.gregs[REG_EFL] &= ~0x100;
    if (g_watch) mprotect(reinterpret_cast<void*>(g_watch), g_watch_len, PROT_NONE);
}

void Watch() {
    const auto trigger = app::ExeDir() / "watch";
    std::ifstream in(trigger);
    if (!in) return;
    std::string where;
    uint32_t len = 0;
    in >> where >> std::hex >> len;
    in.close();
    std::filesystem::remove(trigger);
    if (g_watch) {
        const uintptr_t page = g_watch;
        g_watch = 0;
        mprotect(reinterpret_cast<void*>(page), g_watch_len, PROT_READ | PROT_WRITE);
        if (FILE* f = std::fopen((app::ExeDir() / "watch.txt").c_str(), "w")) {
            for (int i = 0; i < kSlots; i++) {
                if (!g_pcs[i]) continue;
                Dl_info dl{};
                dladdr(reinterpret_cast<void*>(g_pcs[i]), &dl);
                std::fprintf(f, "%s 0x%zx\n", dl.dli_fname ? dl.dli_fname : "?", size_t(g_pcs[i] - uintptr_t(dl.dli_fbase)));
            }
            std::fclose(f);
        }
        LOGR("test: watch off, %d instructions", g_hits.load());
    }
    if (where == "off") return;
    const uint32_t va = Resolve(where);
    if (!va || !mem::IsCommitted(va)) {
        LOGR("test: watch %s not readable", where.c_str());
        return;
    }
    struct sigaction sa{};
    sa.sa_flags = SA_SIGINFO;
    sa.sa_sigaction = OnSegv;
    sigaction(SIGSEGV, &sa, &g_old_segv);
    sa.sa_sigaction = OnTrap;
    sigaction(SIGTRAP, &sa, &g_old_trap);
    g_hits = 0;
    std::memset(g_pcs, 0, sizeof g_pcs);
    const uintptr_t start = reinterpret_cast<uintptr_t>(mem::Base() + va);
    g_watch_len = ((start + std::max<uint32_t>(len, 1) + 4095) & ~uintptr_t(4095)) - (start & ~uintptr_t(4095));
    g_watch = start & ~uintptr_t(4095);
    mprotect(reinterpret_cast<void*>(g_watch), g_watch_len, PROT_NONE);
    LOGR("test: watching guest %08X, %zu KB", va & ~4095u, size_t(g_watch_len >> 10));
}

void Poke() {
    const auto trigger = app::ExeDir() / "poke";
    std::ifstream in(trigger);
    if (!in) return;
    std::string where;
    uint32_t value = 0;
    in >> where >> std::hex >> value;
    in.close();
    std::filesystem::remove(trigger);
    const uint32_t va = Resolve(where);
    if (!va || !mem::IsCommitted(va) || !mem::IsCommitted(va + 3)) {
        LOGR("test: poke %s not writable", where.c_str());
        return;
    }
    LOGR("test: poke %08X: %08X -> %08X", va, kernel::R32(va), value);
    kernel::W32(va, value);
}

void Peek() {
    const auto trigger = app::ExeDir() / "peek";
    std::ifstream in(trigger);
    if (!in) return;
    std::string where, out;
    uint32_t len = 0;
    in >> where >> std::hex >> len >> out;
    in.close();
    std::filesystem::remove(trigger);
    const uint32_t va = Resolve(where);
    if (!va || !mem::IsCommitted(va) || !mem::IsCommitted(va + len - 1)) {
        LOGR("test: peek %s not readable", where.c_str());
        return;
    }
    if (FILE* f = std::fopen((app::ExeDir() / out).c_str(), "wb")) {
        std::fwrite(&va, 4, 1, f);
        std::fwrite(mem::Base() + va, 1, len, f);
        std::fclose(f);
    }
}

}  // namespace

void StartTestHooks() {
    std::thread([] {
        const auto trigger = app::ExeDir() / "dump";
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            PressMenu();
            Peek();
            Poke();
            Watch();
            if (std::ifstream j{app::ExeDir() / "jump"}) {
                std::string v;
                j >> v;
                j.close();
                std::filesystem::remove(app::ExeDir() / "jump");
                cheats::NoteJump(0, v == "1");
            }
            std::ifstream in(trigger);
            if (!in) continue;
            std::string name;
            std::getline(in, name);
            in.close();
            std::filesystem::remove(trigger);
            if (name.empty()) continue;
            FILE* f = std::fopen((app::ExeDir() / name).c_str(), "wb");
            if (!f) continue;
            size_t pages = 0;
            for (uint64_t va = 0x10000; va < 0x100000000ull; va += 0x1000) {
                if (!mem::IsCommitted(uint32_t(va))) continue;
                const uint32_t a = uint32_t(va);
                std::fwrite(&a, 4, 1, f);
                std::fwrite(mem::Base() + va, 1, 0x1000, f);
                pages++;
            }
            std::fclose(f);
            LOGR("memdump: %s, %zu pages", name.c_str(), pages);
        }
    }).detach();
}

}  // namespace port
