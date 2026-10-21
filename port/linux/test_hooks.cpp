// Test bed only, files the run script drops in the run folder:
//   dump  holding an output name: guest memory written there, every committed 4 KB page as its address (4 bytes)
//         then its contents. Used to find the title's variables.
//   menu  holding button names joined by '+' (UP DOWN LEFT RIGHT A B X Y MINUS PLUS): one press on the quick menu
//   peek  "<address or char0/char1> <length> <output>" (hex numbers): that range of guest memory, quickly
//   jump  "1" or "0": player 1 holds jump or lets go, for the moon jump cheat (the pad script's presses are not seen)
#include <chrono>
#include <cstdio>
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

void Peek() {
    const auto trigger = app::ExeDir() / "peek";
    std::ifstream in(trigger);
    if (!in) return;
    std::string where, out;
    uint32_t len = 0;
    in >> where >> std::hex >> len >> out;
    in.close();
    std::filesystem::remove(trigger);
    uint32_t va = 0;
    if (where.rfind("char", 0) == 0) va = kernel::R32(0x5D1CB8 + uint32_t(std::stoi(where.substr(4))) * 0x3238);
    else va = uint32_t(std::stoul(where, nullptr, 16));
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
