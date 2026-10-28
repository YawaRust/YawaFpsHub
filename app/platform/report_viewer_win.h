#pragma once

#include "report/package.h"

#include <windows.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace yawa {

struct ReportViewedCard
{
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> bgra;
};

// WIC декодирует только ограниченную карточку; структурную проверку пакета делает reader.
bool decodeReportCardPng(std::string_view png, ReportViewedCard& image, std::string& error);

// Показывает карточку из проверенного локального пакета без доверия к её утверждениям.
bool showReportPackageViewer(HWND owner, const ReportPackageContents& contents, std::string& error);

} // namespace yawa
