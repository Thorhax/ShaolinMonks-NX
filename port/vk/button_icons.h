#pragma once
// Switch button icons in place of the Xbox ones the title draws. Its menus and prompts take the icons from the
// font atlas (a 256x512 DXT5 texture): the coloured A B X Y, the White and Black buttons and the triggers. The
// port maps buttons by position (the Switch's B is the Xbox's A, both at the bottom), so each icon becomes the
// Switch button in that place. The icons are drawn here, at upload, over the decoded atlas; nothing of the
// title's is kept.
#include <atomic>
#include <cstdint>

namespace vkr::icons {

// the title's font atlas (USA/Europe), by the hash of its guest bytes
bool IsButtonAtlas(uint64_t id);
// draws the Switch icons into the atlas's first level, BGRA
void Paint(uint8_t* bgra, uint32_t width, uint32_t height, uint32_t pitch);

// Switch icons (the default) or the title's own, the quick menu's setting; a change counts up, so the atlas is
// uploaded again
bool SwitchIcons();
uint32_t Epoch();

}  // namespace vkr::icons
