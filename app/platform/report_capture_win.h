#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

struct GameClientFrame
{
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> bgra; // top-down, opaque; only the game client's pixels.
};

// Call only from an explicit user action. Never contacts or injects into the game.
// Does not write a file. Leaves frame unchanged if capture fails.
bool captureForegroundGameClient(std::uint32_t gamePid, GameClientFrame& frame, std::string& error);

} // namespace yawa
