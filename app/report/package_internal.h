#pragma once

#include <cstdint>
#include <string_view>

namespace yawa::report_internal {

std::uint32_t crc32(std::string_view data);
bool validPng(std::string_view png);

} // namespace yawa::report_internal
