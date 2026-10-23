#include "platform/app_paths.h"

#include <windows.h>
#include <shellapi.h>

#include <fstream>
#include <sstream>
#include <vector>

namespace yawa {
namespace {

std::string toUtf8(const std::wstring& text)
{
    std::string result;
    result.reserve(text.size());

    for (std::size_t index = 0; index < text.size(); ++index) {
        std::uint32_t code = text[index];
        if (code >= 0xD800 && code <= 0xDBFF && index + 1 < text.size()) {
            const std::uint32_t low = text[index + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                ++index;
            }
        }

        if (code < 0x80) {
            result.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            result.push_back(static_cast<char>(0xC0 | (code >> 6)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code < 0x10000) {
            result.push_back(static_cast<char>(0xE0 | (code >> 12)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (code >> 18)));
            result.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }
    return result;
}

std::wstring toWide(const std::string& text)
{
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

} // namespace

std::string executableDirectory()
{
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);

    std::wstring full(path);
    const std::size_t slash = full.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        full.resize(slash);
    }
    return toUtf8(full);
}

std::string settingsPath()
{
    return executableDirectory() + "\\YawaFpsHub.ini";
}

std::string logPath()
{
    return executableDirectory() + "\\YawaFpsHub.log";
}

std::string sessionsDirectory()
{
    return executableDirectory() + "\\sessions";
}

bool readTextFile(const std::string& path, std::string& content)
{
    std::ifstream file(toWide(path), std::ios::binary);
    if (!file.is_open()) {
        return false;
    }

    std::ostringstream buffer;
    buffer << file.rdbuf();
    content = buffer.str();
    return true;
}

bool writeTextFile(const std::string& path, const std::string& content)
{
    // Сначала пишем рядом, потом заменяем: оборванная запись не оставит пустой ini.
    const std::string temporary = path + ".tmp";
    {
        std::ofstream file(toWide(temporary), std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return false;
        }
        file << content;
    }

    return MoveFileExW(toWide(temporary).c_str(), toWide(path).c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
}

void ensureDirectory(const std::string& path)
{
    CreateDirectoryW(toWide(path).c_str(), nullptr);
}

void revealInExplorer(const std::string& path)
{
    ShellExecuteW(nullptr, L"open", toWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

} // namespace yawa
