#pragma once

#include "metrics/diagnosis.h"
#include "metrics/report_frame.h"
#include "platform/report_capture_win.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace yawa {

struct ReportCardBitmap
{
    std::vector<std::uint8_t> bgra;
    std::string png;
};

// A 1920x1080 card. The only optional pixels come from an explicit game-client capture.
bool renderReportCard(const GameDiagnosisInput& input, const GameClientFrame* frame,
    std::span<const ReportMask> masks, ReportCardBitmap& result, std::string& error);

} // namespace yawa
