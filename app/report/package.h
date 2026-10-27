#pragma once

#include "metrics/diagnosis.h"

#include <string>
#include <string_view>

namespace yawa {

// Card PNG is the exact image the user previewed before saving. The caller must
// capture a game frame only after an explicit request and render it into that card.
struct ReportPackageInput
{
    GameDiagnosisInput diagnosis;
    std::string_view cardPng;
    bool gameFrameIncluded = false;
};

// Produces an offline, uncompressed ZIP (.yawa). No file or network I/O here.
// On failure, package is unchanged and error contains the reason.
bool buildReportPackage(const ReportPackageInput& input, std::string& package, std::string& error);

} // namespace yawa
