#pragma once

#include <windows.h>

namespace yawa {

// Opens the changelog embedded in the exe; no external file is needed.
bool openEmbeddedChangelog(HWND owner);

} // namespace yawa
