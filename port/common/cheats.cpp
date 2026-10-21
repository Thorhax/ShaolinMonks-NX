#include "cheats.h"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <bit>
#include <atomic>
#include <cstring>
#include <vector>

#include "core/log.h"
#include "cpu/x86_ops.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"

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
// the move list (the pause menu's Moves tab): [0x4EFA88] is its root, a node with a count at +0x0C and its children
// at +0x14 (the root's at +0x10), a text id at +0x18; a character's list is a child of the root, the Fatalities
// heading one of its children (text 0x190), the fatalities that heading's children
constexpr uint32_t kMoveList = 0x4EFA88, kFatalitiesHeading = 0x190;
// the fatalities performed, by their text ids, for every character: the menu shows those and "???" for the rest
// (0x03E890 adds one; it is part of the progress the title saves)
constexpr uint32_t kKnownFatalities = 0x4DF5DC, kKnownFatalityCount = 0x4DFC1C;
constexpr uint32_t kKnownFatalitiesMax = (kKnownFatalityCount - kKnownFatalities) / 4;

// per player; a cheat for the whole game keeps its setting in player 0's
std::array<std::array<std::atomic<bool>, kCount>, kPlayers> g_on{};

std::atomic<bool>& On(int cheat, int player) { return g_on[PerPlayer(cheat) ? size_t(player) : 0][size_t(cheat)]; }
std::array<std::atomic<bool>, 2> g_jump_held{};
// enemies whose health the 10x damage cheat divided, by address, with the maximum it left them
std::unordered_map<uint32_t, uint32_t> g_weakened;
// the unlock bits before "unlock everything", put back when it is turned off
std::array<uint8_t, kUnlockBytes> g_unlock_before{};
bool g_unlock_applied = false;
// the fatalities "unlock all fatalities" added to the known ones, taken out again when it is turned off
std::vector<uint32_t> g_fatalities_added;

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

// every character's fatalities, by text id, from the move list
std::vector<uint32_t> AllFatalities() {
    std::vector<uint32_t> out;
    const auto children = [](uint32_t node, uint32_t list_at, std::vector<uint32_t>& to) {
        const uint32_t n = kernel::R32(node + 0x0C), list = kernel::R32(node + list_at);
        if (n == 0 || n > 64 || !Readable(list, n * 4)) return;
        for (uint32_t i = 0; i < n; i++)
            if (const uint32_t c = kernel::R32(list + i * 4); Readable(c, 0x30)) to.push_back(c);
    };
    const uint32_t root = kernel::R32(kMoveList);
    if (!Readable(root, 0x18)) return out;
    std::vector<uint32_t> characters, headings, moves;
    children(root, 0x10, characters);
    for (uint32_t character : characters) {
        headings.clear();
        children(character, 0x14, headings);
        for (uint32_t h : headings) {
            if (kernel::R32(h + 0x18) != kFatalitiesHeading) continue;
            moves.clear();
            children(h, 0x14, moves);
            for (uint32_t m : moves) out.push_back(kernel::R32(m + 0x18));
        }
    }
    return out;
}

bool KnownFatality(uint32_t text) {
    const uint32_t n = std::min(kernel::R32(kKnownFatalityCount), kKnownFatalitiesMax);
    for (uint32_t i = 0; i < n; i++)
        if (kernel::R32(kKnownFatalities + i * 4) == text) return true;
    return false;
}

// unlock all fatalities: each one not performed yet is added to the known ones (the move list is loaded with the
// first level, so this is done again until it is there); turned off, the ones it added are taken out
void UnlockFatalities(bool on) {
    if (!mem::IsCommitted(kKnownFatalities) || !mem::IsCommitted(kKnownFatalityCount)) return;
    uint32_t n = std::min(kernel::R32(kKnownFatalityCount), kKnownFatalitiesMax);
    if (on) {
        const size_t before = g_fatalities_added.size();
        for (uint32_t text : AllFatalities()) {
            if (KnownFatality(text) || n >= kKnownFatalitiesMax) continue;
            kernel::W32(kKnownFatalities + n * 4, text);
            kernel::W32(kKnownFatalityCount, ++n);
            g_fatalities_added.push_back(text);
        }
        if (g_fatalities_added.size() != before)
            LOGR("cheat: %zu fatalities unlocked", g_fatalities_added.size() - before);
        return;
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t text = kernel::R32(kKnownFatalities + i * 4);
        if (std::find(g_fatalities_added.begin(), g_fatalities_added.end(), text) == g_fatalities_added.end())
            kernel::W32(kKnownFatalities + kept++ * 4, text);
    }
    kernel::W32(kKnownFatalityCount, kept);
    LOGR("cheat: %zu unlocked fatalities taken back", g_fatalities_added.size());
    g_fatalities_added.clear();
}

int PlayerCharacter(uint32_t c) {
    const uint32_t state = GameState();
    if (!c || !state) return -1;
    for (int p = 0; p < Players(state); p++)
        if (Character(p) == c) return p;
    return -1;
}

}  // namespace

const char* Name(int cheat) {
    switch (cheat) {
        case kInvincible: return "Invincibility";
        case kMaxExperience: return "999,999 experience";
        case kUnlockAll: return "Unlock everything";
        case kUnlockFatalities: return "Unlock all fatalities";
        case kEnergy: return "Infinite energy (blue bar)";
        case kFatalityMeter: return "Fatality meter always full";
        case kDamage10x: return "10x damage to enemies";
        case kMoonJump: return "Moon jump (hold jump to rise)";
    }
    return "";
}

bool PerPlayer(int cheat) { return cheat != kUnlockAll && cheat != kUnlockFatalities && cheat != kDamage10x; }

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

int PlayerOf(uint32_t character) { return PlayerCharacter(character); }

void NoteJump(int player, bool held) {
    if (player >= 0 && player < int(g_jump_held.size())) g_jump_held[size_t(player)] = held;
}

void OnFrame(X86Context& ctx) {
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

    if (On(kUnlockFatalities, 0) || !g_fatalities_added.empty()) UnlockFatalities(On(kUnlockFatalities, 0));

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
