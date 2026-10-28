#pragma once

#include "report/package.h"

#include <string>

namespace yawa {

// Один офлайн-файл из содержимого, принятого readReportPackage.
// Карточка встраивается без изменений; текст пакета не трактуется как HTML.
bool buildHtmlReport(const ReportPackageContents& contents, std::string& html, std::string& error);

} // namespace yawa
