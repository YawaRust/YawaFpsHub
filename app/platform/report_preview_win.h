#pragma once

#include "metrics/diagnosis.h"

#include <windows.h>

#include <string>

namespace yawa {

// Modal preview of the exact bytes offered for saving. No file is written on cancel.
bool showReportCardPreview(HWND owner, const GameDiagnosisInput& input, HWND gameWindow, std::string& error);

} // namespace yawa
