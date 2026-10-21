#pragma once
// Testing aids for the quick menu: cheats on the title's own variables, and save slots (copies of the save folder).
//
// The title's variables, from the executable (USA/Europe, XBE SHA-256 48240902...):
//   game state       [0x5ACEBC]; [state + 0x88] is the mode, 1 for Ko-op (two players)
//   experience       [state + (player + 1) * 80 + 0x48F0], player 0 or 1 (the addexperience script command)
//   Fatality meter   float [state + (player + 1) * 80 + 0x4904]; full at [[[0x64D218 + ((player + 1) * 3 + 3) * 4]
//                    + 0x2B6C] + 0x54] + 4 (a float) times the float at 0x49F0F4 (setfatalityAmount, isfatalityfull)
//   energy           the blue bar under health: 0x24 bytes a player from 0x4E6598, the maximum (an int, 100) at
//                    +0x08, the current value (a float) at +0x10, the player's record at +0x20; a special move
//                    costs 20 (found by dumping memory with the bar full, drained and full again)
//   characters       0x508 bytes each from [0x510044], [0x5142A4] of them, in use where the word at [0x51004C] + 2i
//                    is set; the players are the first two. Health +0x368, maximum +0x370 as above
//   character        [0x5D1CB8 + player * 0x3238]; health at +0x368, its maximum at +0x370 (applydamage, and the
//                    health setter at 0x153CD0)
//   player record    0x5D1AB8 + player * 0x3238: height +0x84 (up is negative), vertical speed +0xD4, which the
//                    physics integrates (sampled through a jump: -105 at take-off, 0 at the top, +87 falling)
//   unlock everything 0x0B67A0, a developer routine nothing calls (so it is done here, not recompiled): the unlock
//                    bits at 0x5A375C..0x5A378B and the three cheat code flags at 0x5A378D..0x5A378F
#include <cstdint>
#include <string>

struct X86Context;

namespace port::cheats {

enum Cheat : int { kInvincible, kEnergy, kMaxExperience, kFatalityMeter, kDamage10x, kMoonJump, kUnlockAll, kCount };
const char* Name(int cheat);
constexpr int kPlayers = 2;
// whether a cheat is set for each player on its own; the others (unlocking, enemies' health) act on the whole game
bool PerPlayer(int cheat);
// player 0 or 1; a cheat for the whole game has one setting, whichever player is asked about
bool Enabled(int cheat, int player);
void Toggle(int cheat, int player);

constexpr int kSlots = 4;
// "Empty", or when the slot was saved
std::string SlotInfo(int slot);
void SaveSlot(int slot);  // queued: carried out on the title's thread between frames
void LoadSlot(int slot);

// whether a player holds the jump button (the Xbox's A), from the host's input
void NoteJump(int player, bool held);

// the player (0 or 1) controlling a character in this game, -1 for none
int PlayerOf(uint32_t character);

// from the title's thread once per frame (D3DDevice_Swap)
void OnFrame(X86Context& ctx);

}  // namespace port::cheats
