#include "platform/app_paths.h"

#include <windows.h>
#include <objbase.h>
#include <knownfolders.h>
#include <shlobj.h>
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

namespace {

// Каталог данных: %LOCALAPPDATA%\\YawaFpsHub. EXE при этом остаётся единственным
// файлом программы — его можно положить куда угодно и запустить оттуда.
std::wstring dataDirectoryWide()
{
    PWSTR known = nullptr;
    std::wstring base;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known != nullptr) {
        base = known;
        CoTaskMemFree(known);
    }
    if (base.empty()) {
        // Локальный профиль недоступен (редкость: портативный запуск без профиля):
        // откатываемся на каталог рядом с exe, чтобы настройки не потерялись.
        wchar_t path[MAX_PATH]{};
        if (FAILED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, path))) {
            return {};
        }
        base = path;
    }
    while (!base.empty() && (base.back() == L'\\' || base.back() == L'/')) {
        base.pop_back();
    }
    return base + L"\\YawaFpsHub";
}

// Старые версии держали ini рядом с exe. Переносим файл один раз: пользователь
// не должен проснуться с настройками по умолчанию после обновления.
void migrateLegacyFile(const std::wstring& from, const std::wstring& to)
{
    if (from.empty() || GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if (GetFileAttributesW(from.c_str()) != INVALID_FILE_ATTRIBUTES) {
        CopyFileW(from.c_str(), to.c_str(), TRUE);
    }
}

} // namespace

std::string settingsPath()
{
    const std::wstring directory = dataDirectoryWide();
    if (directory.empty()) {
        return executableDirectory() + "\\YawaFpsHub.ini";
    }
    ensureDirectory(toUtf8(directory));
    const std::wstring target = directory + L"\\YawaFpsHub.ini";
    migrateLegacyFile(toWide(executableDirectory() + "\\YawaFpsHub.ini"), target);
    return toUtf8(target);
}

std::string logPath()
{
    const std::wstring directory = dataDirectoryWide();
    if (directory.empty()) {
        return executableDirectory() + "\\YawaFpsHub.log";
    }
    ensureDirectory(toUtf8(directory));
    const std::wstring target = directory + L"\\YawaFpsHub.log";
    migrateLegacyFile(toWide(executableDirectory() + "\\YawaFpsHub.log"), target);
    return toUtf8(target);
}

std::string sessionsDirectory()
{
    const std::wstring directory = dataDirectoryWide();
    if (directory.empty()) {
        return executableDirectory() + "\\sessions";
    }
    ensureDirectory(toUtf8(directory));
    return toUtf8(directory + L"\\sessions");
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
