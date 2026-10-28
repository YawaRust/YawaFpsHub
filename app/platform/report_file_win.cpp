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

} // namespace yawa
