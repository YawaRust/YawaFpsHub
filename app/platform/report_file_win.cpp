#include "platform/report_file_win.h"
#include "platform/win_util.h"

#include <windows.h>

#include <utility>

namespace yawa {

bool readLocalReport(std::wstring_view path, std::string& archive, std::string& error)
{
    error.clear();
    if (path.empty()) {
        error = "empty path is not accepted";
        return false;
    }
    if (path.starts_with(L"\\\\") || path.starts_with(L"//")) {
        error = "network paths are not accepted";
        return false;
    }
    const HANDLE file = CreateFileW(std::wstring(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = "cannot open " + narrow(path);
        return false;
    }
    LARGE_INTEGER length { };
    constexpr LONGLONG kMaxArchiveBytes = 33LL * 1024 * 1024;
    const bool sized
        = GetFileSizeEx(file, &length) && length.QuadPart > 0 && length.QuadPart <= kMaxArchiveBytes;
    DWORD received = 0;
    if (sized) {
        std::string bytes(static_cast<std::size_t>(length.QuadPart), '\0');
        if (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr)
            && received == bytes.size()) {
            archive = std::move(bytes);
        } else {
            error = "file could not be read completely";
        }
    } else {
        error = "file is empty, unreadable or larger than 33 MiB";
    }
    CloseHandle(file);
    return error.empty();
}

bool writeLocalHtmlReport(std::wstring_view path, std::string_view html, std::string& error)
{
    error.clear();
    if (path.empty() || path.starts_with(L"\\\\") || path.starts_with(L"//") || html.empty()
        || html.size() > 50u * 1024u * 1024u) {
        error = "HTML-отчёт: неверный локальный путь или размер файла";
        return false;
    }
    const std::wstring target(path);
    wchar_t directory[MAX_PATH] { };
    wchar_t* basename = nullptr;
    const DWORD length = GetFullPathNameW(target.c_str(), MAX_PATH, directory, &basename);
    if (length == 0 || length >= MAX_PATH || basename == nullptr) {
        error = "HTML-отчёт: путь слишком длинный или недоступен";
        return false;
    }
    *basename = L'\0';
    wchar_t temporary[MAX_PATH] { };
    if (GetTempFileNameW(directory, L"YWH", 0, temporary) == 0) {
        error = "HTML-отчёт: не удалось создать временный файл";
        return false;
    }
    HANDLE file
        = CreateFileW(temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        DeleteFileW(temporary);
        error = "HTML-отчёт: временный файл недоступен";
        return false;
    }
    DWORD written = 0;
    const bool complete = WriteFile(file, html.data(), static_cast<DWORD>(html.size()), &written, nullptr)
        && written == html.size() && FlushFileBuffers(file) != FALSE;
    const bool closed = CloseHandle(file) != FALSE;
    const bool moved
        = complete && closed && MoveFileExW(temporary, target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!moved) {
        DeleteFileW(temporary);
        error = "HTML-отчёт: не удалось записать файл (возможно, он уже существует): " + narrow(path);
    }
    return moved;
}

} // namespace yawa
