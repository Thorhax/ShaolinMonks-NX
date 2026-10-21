#include "cheats.h"

#include <array>
#include <unordered_map>
#include <bit>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

namespace fs = std::filesystem;

namespace port::cheats {
namespace {

constexpr uint32_t kGameState = 0x5ACEBC;
constexpr uint32_t kCharacters = 0x5D1CB8, kCharacterStride = 0x3238;
constexpr uint32_t kHealth = 0x368, kHealthMax = 0x370;
// the character array, and the Fatality meter's full value
constexpr uint32_t kCharacterArray = 0x510044, kCharacterFlags = 0x51004C, kCharacterCount = 0x5142A4;
constexpr uint32_t kCharacterSize = 0x508;
constexpr uint32_t kPlayerObjects = 0x64D218, kFatalityScale = 0x49F0F4;
constexpr uint32_t kEnergyBlocks = 0x4E6598, kEnergyStride = 0x24, kEnergyMax = 0x08, kEnergyNow = 0x10;

// a player's record, which the character lookup is keyed on: its vertical speed is what the physics integrates
// (the character's own height and speed fields are copies of the result)
constexpr uint32_t kPlayerRecords = 0x5D1AB8, kRecordVerticalSpeed = 0xD4;
constexpr float kMoonJumpSpeed = -60.0f;  // upward; a jump starts at -105
constexpr uint32_t kBitMasks = 0x4FC348;  // the title's table of 1 << n
constexpr uint32_t kUnlockBits = 0x5A375C, kUnlockBytes = 0x5A3790 - kUnlockBits;
constexpr uint32_t kExperienceValue = 999999;

// per player; a cheat for the whole game keeps its setting in player 0's
std::array<std::array<std::atomic<bool>, kCount>, kPlayers> g_on{};

std::atomic<bool>& On(int cheat, int player) { return g_on[PerPlayer(cheat) ? size_t(player) : 0][size_t(cheat)]; }
std::array<std::atomic<bool>, 2> g_jump_held{};
// enemies whose health the 10x damage cheat divided, by address, with the maximum it left them
std::unordered_map<uint32_t, uint32_t> g_weakened;
// the unlock bits before "unlock everything", put back when it is turned off
std::array<uint8_t, kUnlockBytes> g_unlock_before{};
bool g_unlock_applied = false;

std::mutex g_queue_lock;
std::vector<int> g_saves, g_loads;  // slots, 0 based

bool Readable(uint32_t va, uint32_t size) { return va >= 0x10000 && mem::IsCommitted(va) && mem::IsCommitted(va + size - 1); }

uint32_t GameState() {
    const uint32_t s = kernel::R32(kGameState);
    return Readable(s, 0x4A00) ? s : 0;
}

// players in this game: two in Ko-op, else one
int Players(uint32_t state) { return kernel::R32(state + 0x88) == 1 ? 2 : 1; }

uint32_t Character(int player) {
    const uint32_t c = kernel::R32(kCharacters + uint32_t(player) * kCharacterStride);
    return Readable(c, kHealthMax + 4) ? c : 0;
}

// what the developers' routine at 0x0B67A0 does (it is never called, so it was not recompiled): the unlock bits,
// then the three cheat code flags
void UnlockEverything() {
    const auto set_bit = [](uint32_t k) {
        const uint32_t word = kUnlockBits + (k >> 5) * 4;
        kernel::W32(word, kernel::R32(word) | kernel::R32(kBitMasks + (k & 31) * 4));
    };
    for (uint32_t k : {18u, 19u, 20u, 24u, 25u, 26u, 28u, 29u, 30u, 31u}) set_bit(2 * 32 + k);
    for (uint32_t k : {0u, 1u, 2u, 3u}) set_bit(3 * 32 + k);
    for (uint32_t k = 14; k <= 22; k++) set_bit(11 * 32 + k);
    for (uint32_t k = 101; k <= 164; k++) set_bit(k);
    for (uint32_t k = 166; k <= 341; k++) set_bit(k);
    for (uint32_t k = 343; k <= 358; k++) set_bit(k);
    for (uint32_t flag : {0x5A378Du, 0x5A378Eu, 0x5A378Fu}) kernel::G<uint8_t>(flag) = 1;
}

float FatalityFull(int player) {
    const uint32_t p = kernel::R32(kPlayerObjects + uint32_t((player + 1) * 3 + 3) * 4);
    if (!Readable(p, 0x2B70)) return 0;
    const uint32_t a = kernel::R32(p + 0x2B6C);
    if (!Readable(a, 0x58)) return 0;
    const uint32_t b = kernel::R32(a + 0x54);
    if (!Readable(b, 8)) return 0;
    return std::bit_cast<float>(kernel::R32(b + 4)) * std::bit_cast<float>(kernel::R32(kFatalityScale));
}

// 10x damage: every enemy has a tenth of its health, so each hit takes ten times as much of it. Turned off, the
// enemies still standing get it back.
void WeakenEnemies(bool on) {
    const uint32_t array = kernel::R32(kCharacterArray), flags = kernel::R32(kCharacterFlags);
    const uint32_t count = std::min<uint32_t>(kernel::R32(kCharacterCount), 64);
    if (!on) {
        for (const auto& [c, max] : g_weakened)
            if (Readable(c, kHealthMax + 4) && kernel::R32(c + kHealthMax) == max) {
                kernel::W32(c + kHealthMax, max * 10);
                kernel::W32(c + kHealth, kernel::R32(c + kHealth) * 10);
            }
        g_weakened.clear();
        return;
    }
    if (!Readable(array, kCharacterSize * count) || !Readable(flags, count * 2)) return;
    for (uint32_t i = 2; i < count; i++) {  // 0 and 1 are the players
        const uint32_t c = array + i * kCharacterSize;
        if (!kernel::G<uint16_t>(flags + i * 2)) {
            g_weakened.erase(c);
            continue;
        }
        const uint32_t max = kernel::R32(c + kHealthMax), health = kernel::R32(c + kHealth);
        if (int32_t(max) <= 0 || int32_t(health) <= 0) continue;
        auto it = g_weakened.find(c);
        if (it != g_weakened.end() && it->second == max) continue;  // done, and not spawned again since
        const uint32_t weak = std::max<uint32_t>(1, max / 10);
        kernel::W32(c + kHealthMax, weak);
        kernel::W32(c + kHealth, std::max<uint32_t>(1, (health + 9) / 10));
        g_weakened[c] = weak;
    }
}

int PlayerCharacter(uint32_t c) {
    const uint32_t state = GameState();
    if (!c || !state) return -1;
    for (int p = 0; p < Players(state); p++)
        if (Character(p) == c) return p;
    return -1;
}

// --- save slots: the title's whole folder on the virtual hard disk, copied file by file (std::filesystem::copy
// would set permissions, which Horizon's SD card does not have)

fs::path SaveFolder() { return app::ExeDir() / "hdd" / "E" / "UDATA" / "4d570029"; }
fs::path SlotFolder(int slot) { return app::ExeDir() / "slots" / std::to_string(slot + 1); }

bool CopyFile(const fs::path& from, const fs::path& to) {
    FILE* in = std::fopen(from.c_str(), "rb");
    if (!in) return false;
    FILE* out = std::fopen(to.c_str(), "wb");
    bool ok = out != nullptr;
    char buf[16384];
    for (size_t n; ok && (n = std::fread(buf, 1, sizeof buf, in)) != 0;) ok = std::fwrite(buf, 1, n, out) == n;
    ok = ok && !std::ferror(in);
    std::fclose(in);
    if (out && std::fclose(out)) ok = false;
    return ok;
}

bool CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return false;
    for (auto it = fs::directory_iterator(from, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path target = to / it->path().filename();
        const bool ok = it->is_directory(ec) ? CopyTree(it->path(), target) : CopyFile(it->path(), target);
        if (!ok) return false;
    }
    return !ec;
}

// The title's save folders (one per profile, e.g. 80482ECC5634) are what a slot keeps. The files beside them
// (TitleMeta.xbx and the images) never change, and the title holds them open, which Horizon will not let go.
std::vector<fs::path> SaveFolders(const fs::path& root) {
    std::vector<fs::path> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (it->is_directory(ec)) out.push_back(it->path());
    return out;
}

void DoSave(int slot) {
    std::error_code ec;
    const fs::path dir = SlotFolder(slot), staging = dir.string() + ".new";
    fs::remove_all(staging, ec);
    const auto folders = SaveFolders(SaveFolder());
    if (folders.empty()) {
        LOGW("save slot %d: the game has not saved yet (%s)", slot + 1, SaveFolder().c_str());
        return;
    }
    for (const auto& f : folders)
        if (!CopyTree(f, staging / f.filename())) {
            LOGE("save slot %d: copy failed (errno %d)", slot + 1, errno);
            fs::remove_all(staging, ec);
            return;
        }
    fs::remove_all(dir, ec);
    fs::rename(staging, dir, ec);
    LOGR("save slot %d: saved %zu save folder(s)", slot + 1, folders.size());
}

void DoLoad(int slot) {
    std::error_code ec;
    const fs::path live = SaveFolder();
    fs::remove_all(live.string() + ".old", ec);  // left by the first version of this
    const auto kept = SaveFolders(SlotFolder(slot));
    if (kept.empty()) return;
    for (const auto& f : SaveFolders(live)) {
        bool in_slot = false;
        for (const auto& k : kept) in_slot = in_slot || k.filename() == f.filename();
        if (!in_slot) fs::remove_all(f, ec);
    }
    for (const auto& k : kept)
        if (!CopyTree(k, live / k.filename())) {
            LOGE("save slot %d: restore of %s failed (errno %d)", slot + 1, k.filename().c_str(), errno);
            return;
        }
    LOGR("save slot %d: restored %zu save folder(s)", slot + 1, kept.size());
}

// when the slot's newest file was written: Horizon keeps no times for folders
std::filesystem::file_time_type SlotTime(int slot, std::error_code& ec) {
    std::filesystem::file_time_type newest{};
    bool any = false;
    for (auto it = fs::recursive_directory_iterator(SlotFolder(slot), ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        const auto t = fs::last_write_time(it->path(), e);
        if (!e && (!any || t > newest)) newest = t;
        any = any || !e;
    }
    if (!ec && !any) ec = std::make_error_code(std::errc::no_such_file_or_directory);
    return newest;
}

}  // namespace

const char* Name(int cheat) {
    switch (cheat) {
        case kInvincible: return "Invincibility";
        case kMaxExperience: return "999,999 experience";
        case kUnlockAll: return "Unlock everything";
        case kEnergy: return "Infinite energy (blue bar)";
        case kFatalityMeter: return "Fatality meter always full";
        case kDamage10x: return "10x damage to enemies";
        case kMoonJump: return "Moon jump (hold jump to rise)";
    }
    return "";
}

bool PerPlayer(int cheat) { return cheat != kUnlockAll && cheat != kDamage10x; }

bool Enabled(int cheat, int player) {
    return cheat >= 0 && cheat < kCount && player >= 0 && player < kPlayers && On(cheat, player).load();
}

void Toggle(int cheat, int player) {
    if (cheat < 0 || cheat >= kCount || player < 0 || player >= kPlayers) return;
    const bool on = !On(cheat, player).load();
    On(cheat, player) = on;
    if (PerPlayer(cheat)) LOGR("cheat: %s %s for player %d", Name(cheat), on ? "on" : "off", player + 1);
    else LOGR("cheat: %s %s", Name(cheat), on ? "on" : "off");
}

std::string SlotInfo(int slot) {
    std::error_code ec;
    const auto t = SlotTime(slot, ec);
    if (ec) return "Empty";
    const auto sys = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    const std::time_t when = std::chrono::system_clock::to_time_t(sys);
    char text[64];
    std::strftime(text, sizeof text, "%Y-%m-%d %H:%M", std::localtime(&when));
    return text;
}

void SaveSlot(int slot) {
    std::lock_guard<std::mutex> lock(g_queue_lock);
    g_saves.push_back(slot);
}

void LoadSlot(int slot) {
    std::lock_guard<std::mutex> lock(g_queue_lock);
    g_loads.push_back(slot);
}

int PlayerOf(uint32_t character) { return PlayerCharacter(character); }

void NoteJump(int player, bool held) {
    if (player >= 0 && player < int(g_jump_held.size())) g_jump_held[size_t(player)] = held;
}

void OnFrame(X86Context& ctx) {
    {
        std::vector<int> saves, loads;
        {
            std::lock_guard<std::mutex> lock(g_queue_lock);
            saves.swap(g_saves);
            loads.swap(g_loads);
        }
        for (int s : saves) DoSave(s);
        for (int s : loads) DoLoad(s);
    }

    // unlock everything: the title's own developer routine; turning it off puts the bits back as they were
    const bool unlock = On(kUnlockAll, 0).load();
    if (unlock != g_unlock_applied && mem::IsCommitted(kUnlockBits)) {
        if (unlock) {
            std::memcpy(g_unlock_before.data(), kernel::HostPtr(kUnlockBits), kUnlockBytes);
            UnlockEverything();
        } else {
            std::memcpy(kernel::HostPtr(kUnlockBits), g_unlock_before.data(), kUnlockBytes);
        }
        g_unlock_applied = unlock;
        LOGR("cheat: unlock bits %s", unlock ? "set" : "restored");
    }

    const uint32_t state = GameState();
    if (!state) return;
    if (On(kDamage10x, 0) || !g_weakened.empty()) WeakenEnemies(On(kDamage10x, 0));
    for (int p = 0; p < Players(state); p++) {
        if (On(kMaxExperience, p)) kernel::W32(state + uint32_t(p + 1) * 80 + 0x48F0, kExperienceValue);
        if (On(kEnergy, p)) {
            const uint32_t e = kEnergyBlocks + uint32_t(p) * kEnergyStride;
            const int32_t max = int32_t(kernel::R32(e + kEnergyMax));
            if (max > 0 && max <= 10000) kernel::W32(e + kEnergyNow, std::bit_cast<uint32_t>(float(max)));
        }
        if (On(kFatalityMeter, p))
            if (const float full = FatalityFull(p); full > 0)
                kernel::W32(state + uint32_t(p + 1) * 80 + 0x4904, std::bit_cast<uint32_t>(full));
        const uint32_t c = Character(p);
        if (!c) continue;
        if (On(kInvincible, p)) kernel::W32(c + kHealth, kernel::R32(c + kHealthMax));
        // Moon jump: while the button is held the player's vertical speed stays upward, and the title's own
        // physics moves and collides the character (a jump starts at -105, gravity takes about 96 off in 0.2 s)
        if (On(kMoonJump, p) && g_jump_held[size_t(p)])
            kernel::W32(kPlayerRecords + uint32_t(p) * kCharacterStride + kRecordVerticalSpeed,
                        std::bit_cast<uint32_t>(kMoonJumpSpeed));
    }
}

}  // namespace port::cheats

// the title's health setter (thiscall: the character in ecx, the new value on the stack): a player's health does
// not go down while invincible, so no hit within a frame can bring it to zero
X86_EXTERN_FUNC(__imp__sub_00153CD0);
X86_FUNC(sub_00153CD0) {
    const int player = port::cheats::PlayerOf(ctx.ecx);
    if (player >= 0 && port::cheats::Enabled(port::cheats::kInvincible, player)) {
        const uint32_t max = kernel::R32(ctx.ecx + 0x370);
        if (kernel::R32(ctx.esp + 4) < max) kernel::W32(ctx.esp + 4, max);
    }
    __imp__sub_00153CD0(ctx, base);
}
