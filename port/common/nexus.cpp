// The title's own debug menu, the Nexus, which is how Survival Mode is reached: upstream's ed_nexus_hooks.inl, less
// what belongs to upstream's in-game settings (its Konfigure Recompilation and Exit to Desktop pause rows).
//
// Holding White (LB) on the main menu, the ten-button sequence in ed_nexus_sequence.h unlocks the Nexus for the
// session. The gameplay pause menu then has a Nexus row after the title's own three, and its shortcut opens it too.
// The Nexus gets an Unlock All Kontent row, which lasts the session and never touches the save.
#include <string>

#include "core/log.h"
#include "cpu/x86_context.h"
#include "ed_nexus.h"
#include "ed_nexus_sequence.h"
#include "kernel/kernel.h"
#include "launcher.h"
#include "menu_text.h"

namespace openshao::ed_nexus {
namespace {
Sequence sequence;
bool kontent_unlocked = false;
uint32_t kontent_menu = 0;
constexpr uint32_t MenuLabel = 0x19F;
constexpr uint32_t KontentLabel = 0xFFFFFFFE;
constexpr uint32_t KontentRow = 7;
constexpr uint32_t KontentListBytes = 9 * 8;
constexpr uint32_t DebugTextBytes = 24;
// the title's three pause rows, the Nexus and the terminator
constexpr uint32_t PauseRows = 4;
constexpr uint32_t PauseRootBytes = (PauseRows + 1) * 8;
uint32_t pause_storage = 0;

void WriteGuestText(uint32_t address, const char* text, unsigned bytes) {
    for (unsigned word = 0; word < bytes / 4; ++word) {
        uint32_t value = 0;
        for (unsigned byte = 0; byte < 4 && *text; ++byte) value |= uint32_t(uint8_t(*text++)) << (byte * 8);
        kernel::W32(address + word * 4, value);
    }
}
// the guest allocation keeps the title's text pointers valid across draws
void WriteMenuText(uint32_t address, const std::string& text) { WriteGuestText(address, text.c_str(), DebugTextBytes); }
// the shared catalog's text in the title's font encoding, as upstream's pause pages write it
std::string Localized(std::string_view id) {
    const auto& locale = launcher::Current().language;
    return menu::EncodeNative(menu::Catalog::Builtin().Get(locale, id), locale == "pt-BR");
}
void PausePair(uint32_t address, uint32_t label, uint32_t callback = 0x135C1) {
    kernel::W32(address, label);
    kernel::W32(address + 4, callback);
}
void PauseTerminator(uint32_t address) { PausePair(address, 0xFFFFFFFF, 0); }

bool EnsureMenuStorage() {
    if (!kontent_menu) {
        kontent_menu = mem::AllocSystem(KontentListBytes + 3 * DebugTextBytes);
        if (!kontent_menu) {
            LOGI("Cannot allocate debug menu storage");
            return false;
        }
    }
    // each caption has its own terminated storage, translations longer than the original "Nexus" included
    WriteMenuText(kontent_menu + KontentListBytes, Localized("debug.unlock_kontent"));
    WriteMenuText(kontent_menu + KontentListBytes + DebugTextBytes, Localized("debug.kontent_unlocked"));
    WriteMenuText(kontent_menu + KontentListBytes + 2 * DebugTextBytes, Localized("debug.menu"));
    return true;
}

struct PauseMenuState {
    uint32_t list = 0, original_rows = 0, original_count = 0;
};
PauseMenuState pause_menu;

// the title's three pause rows and the Nexus after them, once it is unlocked
bool InstallPauseList(X86Context& ctx) {
    if (!Unlocked()) return false;
    const auto saved = ctx;
    ctx.ecx = 0x4EF3C8;
    const uint32_t list = kernel::CallGuest(ctx, 0x1120D, {0x49F924});
    ctx = saved;
    if (!list) {
        LOGI("Gameplay pause expansion skipped: Select List 49F924 not found");
        return false;
    }
    const uint32_t original_rows = kernel::R32(list + 0x54);
    const uint32_t original_count = kernel::R32(list + 0x64);
    // the retail builder copies the rows at 0x4F1CC0 into the list's own buffer
    bool original_valid = original_rows != 0 && original_count == 3;
    if (original_valid) {
        for (unsigned i = 0; i < 6; ++i)
            if (kernel::R32(original_rows + i * 4) != kernel::R32(0x4F1CC0 + i * 4)) original_valid = false;
        if (kernel::R32(original_rows + 24) != 0xFFFFFFFF || kernel::R32(original_rows + 28) != 0) original_valid = false;
    }
    if (!original_valid) {
        LOGI("Gameplay pause expansion skipped: list=%08X buffer=%08X count=%u first=%08X", list, original_rows,
             original_count, original_rows ? kernel::R32(original_rows) : 0);
        return false;
    }
    if (!pause_storage) pause_storage = mem::AllocSystem(PauseRootBytes);
    if (!pause_storage || !EnsureMenuStorage()) return false;
    pause_menu = {list, original_rows, original_count};
    for (unsigned i = 0; i < 6; ++i) kernel::W32(pause_storage + i * 4, kernel::R32(original_rows + i * 4));
    PausePair(pause_storage + 24, MenuLabel, 0x135C6);
    PauseTerminator(pause_storage + PauseRows * 8);
    kernel::W32(list + 0x54, pause_storage);
    kernel::W32(list + 0x64, PauseRows);
    LOGI("Gameplay pause expanded: list=%08X rows=%u", list, PauseRows);
    return true;
}

void AddKontentAction(X86Context& ctx) {
    const auto saved = ctx;
    ctx.ecx = 0x4EF3C8;
    const uint32_t list = kernel::CallGuest(ctx, 0x1120D, {0x49F8FC});
    ctx = saved;
    // the seven native rows cloned: extending their definition would overwrite the object after it
    if (!list || kernel::R32(list + 0x54) != 0x4F1F00 || kernel::R32(list + 0x64) != 7 || kernel::R32(0x4F1F38) != 0xFFFFFFFF)
        return;
    if (!EnsureMenuStorage()) return;
    for (unsigned i = 0; i < 14; ++i) kernel::W32(kontent_menu + i * 4, kernel::R32(0x4F1F00 + i * 4));
    kernel::W32(kontent_menu + 56, KontentLabel);
    kernel::W32(kontent_menu + 60, 0x135C1);  // the retail no-op; accept is handled in sub_00013B4D
    kernel::W32(kontent_menu + 64, 0xFFFFFFFF);
    kernel::W32(kontent_menu + 68, 0);
    kernel::W32(list + 0x54, kontent_menu);
    kernel::W32(list + 0x64, 8);
}

bool in_pause = false, in_nexus = false;
struct PauseScope {
    bool previous_pause = in_pause, previous_nexus = in_nexus;
    PauseMenuState previous_menu = pause_menu;
    PauseScope() {
        in_pause = true;
        in_nexus = false;
        pause_menu = {};
    }
    ~PauseScope() {
        if (pause_menu.list && pause_storage && kernel::R32(pause_menu.list + 0x54) == pause_storage) {
            kernel::W32(pause_menu.list + 0x54, pause_menu.original_rows);
            kernel::W32(pause_menu.list + 0x64, pause_menu.original_count);
            kernel::W32(pause_menu.list + 0x6C, 0);
        }
        pause_menu = previous_menu;
        in_pause = previous_pause;
        in_nexus = previous_nexus;
    }
};

void UpdateHeldInput(X86Context& ctx) {
    const uint32_t depth = kernel::R32(0x6C9308);
    bool main = depth && depth <= 16 && kernel::R32(0x6C9308 + depth * 4) == 0x598430;
    if (main) {
        const uint32_t panel = kernel::R32(0x59843C);
        const uint32_t state = panel ? kernel::R32(panel + 0x2E4) : ~0u;
        main = state == 0 || state == 2;
    }
    // White is held through the sequence, not one of its ten buttons
    const uint16_t pressed = uint16_t(kernel::R32(0x67C66A)) & ~uint16_t{1};
    const bool modifier = main && (kernel::R32(0x67C668) & 1);
    if (!sequence.Feed(pressed, main, modifier)) return;
    const auto saved = ctx;
    ctx.ecx = 0x653BC0;
    // the loaded shared voice bank's vo_sk_combo_counter_190_1.adx, as confirmation
    const uint32_t voice = kernel::CallGuest(ctx, 0x151560, {0x5CE79082, 0x3F800000, 0, 0, 0, 0xBF800000, 0, 0});
    ctx = saved;
    LOGR("Nexus unlocked for this session (voice=%08X)", voice);
}
}  // namespace

bool Unlocked() { return sequence.Unlocked(); }
void FrontendInput(X86Context& ctx, uint8_t*) {
    // the main menu still ticks with empty input while the held White path runs
    if (ctx.ecx != 0x598430 || !(kernel::R32(0x67C668) & 1)) sequence.ResetInput();
}
}  // namespace openshao::ed_nexus

namespace nexus = openshao::ed_nexus;

X86_EXTERN_FUNC(__imp__sub_00012BA8);
X86_EXTERN_FUNC(__imp__sub_0001332D);
X86_FUNC(sub_0001332D) {
    __imp__sub_0001332D(ctx, base);
    if (nexus::in_pause) nexus::InstallPauseList(ctx);
}
X86_EXTERN_FUNC(__imp__sub_000135C6);
X86_FUNC(sub_000135C6) {
    const bool gameplay_nexus = nexus::in_pause && nexus::Unlocked();
    if (gameplay_nexus) nexus::in_nexus = true;
    __imp__sub_000135C6(ctx, base);
    if (gameplay_nexus) {
        nexus::AddKontentAction(ctx);
        LOGI("Nexus opened from gameplay pause");
    }
}
X86_EXTERN_FUNC(__imp__sub_001B5300);
X86_FUNC(sub_001B5300) {
    // holding the title's modifier bypasses the main menu's usual callback
    if (ctx.ecx == 0x6CA3A4) nexus::UpdateHeldInput(ctx);
    __imp__sub_001B5300(ctx, base);
}
X86_FUNC(sub_00012BA8) {
    nexus::PauseScope pause;
    __imp__sub_00012BA8(ctx, base);
}
X86_EXTERN_FUNC(__imp__sub_000142EB);
X86_FUNC(sub_000142EB) {
    const bool gameplay_gui = ctx.ecx == 0x4EF3C8;
    __imp__sub_000142EB(ctx, base);
    if (!gameplay_gui ||
        !nexus::OpenShortcut(nexus::Unlocked(), nexus::in_pause, nexus::in_nexus, uint16_t(kernel::R32(0x67C66A))))
        return;
    const auto saved = ctx;
    kernel::CallGuest(ctx, 0x135C6, {});
    ctx = saved;
}
X86_EXTERN_FUNC(__imp__sub_00013B4D);
X86_FUNC(sub_00013B4D) {
    const uint32_t list = kernel::R32(ctx.esp + 4), event = kernel::R32(ctx.esp + 8);
    if (nexus::Unlocked() && nexus::in_nexus && event == 4 && list && nexus::kontent_menu &&
        kernel::R32(list + 0x54) == nexus::kontent_menu && kernel::R32(list + 0x64) == 8 &&
        kernel::R32(list + 0x6C) == nexus::KontentRow) {
        // the session's availability only: the flags the title saves stay as they are
        nexus::kontent_unlocked = true;
        LOGR("All Kontent unlocked for this session; native save flags unchanged");
        kernel::Return(ctx, 1, 0);
        return;
    }
    // event 6 in the Nexus list's handler goes back to the usual pause
    const bool cancel = event == 6;
    __imp__sub_00013B4D(ctx, base);
    if (cancel) nexus::in_nexus = false;
}

// the gallery's predicates return AL with caller-cleaned arguments; other registers and bad indices stay as they are
#define NEXUS_KONTENT_QUERY(name, count)                                                       \
    X86_EXTERN_FUNC(__imp__##name);                                                            \
    X86_FUNC(name) {                                                                           \
        const uint32_t index = kernel::R32(ctx.esp + 4);                                       \
        __imp__##name(ctx, base);                                                              \
        if (nexus::kontent_unlocked && index < count) ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1;   \
    }
NEXUS_KONTENT_QUERY(sub_000160AF, 64)   // arenas
NEXUS_KONTENT_QUERY(sub_00011F64, 176)  // concept art
NEXUS_KONTENT_QUERY(sub_00013FCB, 8)    // movies
NEXUS_KONTENT_QUERY(sub_00013751, 16)   // photos
#undef NEXUS_KONTENT_QUERY
X86_EXTERN_FUNC(__imp__sub_00015C6D);
X86_FUNC(sub_00015C6D) {
    __imp__sub_00015C6D(ctx, base);
    if (nexus::kontent_unlocked) ctx.eax = (ctx.eax & 0xFFFFFF00u) | 1;
}
X86_EXTERN_FUNC(__imp__sub_00016992);
X86_FUNC(sub_00016992) {
    const uint32_t label = kernel::R32(ctx.esp + 4);
    if (nexus::kontent_menu && label == nexus::MenuLabel) {
        kernel::Return(ctx, nexus::kontent_menu + nexus::KontentListBytes + 2 * nexus::DebugTextBytes, 0);
        return;
    }
    if (nexus::kontent_menu && label == nexus::KontentLabel) {
        kernel::Return(ctx, nexus::kontent_menu + nexus::KontentListBytes + (nexus::kontent_unlocked ? nexus::DebugTextBytes : 0), 0);
        return;
    }
    __imp__sub_00016992(ctx, base);
}
