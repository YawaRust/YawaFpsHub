#pragma once

#include "metrics/diagnosis.h"

#include <cstdint>
#include <string>
#include <vector>

namespace yawa {

struct ReportCardBitmap
{
    std::vector<std::uint8_t> bgra;
    std::string png;
};

// A 1920x1080 card rendered separately from the app and desktop; no game frame.
bool renderReportCard(const GameDiagnosisInput& input, ReportCardBitmap& result, std::string& error);

} // namespace yawa
