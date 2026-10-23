#include "platform/win_util.h"

#include <windows.h>

namespace yawa {
namespace {

std::wstring systemMessage(unsigned long errorCode)
{
    wchar_t* buffer = nullptr;
    const unsigned long size = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);

    std::wstring message;
    if (size != 0 && buffer != nullptr) {
        message.assign(buffer, size);
        while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) {
            message.pop_back();
        }
    }
    LocalFree(buffer);
    return message;
}

} // namespace

std::string narrow(std::wstring_view text)
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

std::wstring widen(std::string_view text)
{
    if (text.empty()) {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }

    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string win32ErrorText(unsigned long errorCode)
{
    const std::wstring message = systemMessage(errorCode);
    if (message.empty()) {
        return "код " + std::to_string(errorCode);
    }
    return narrow(message) + " (" + std::to_string(errorCode) + ")";
}

std::string hresultText(long result)
{
    const unsigned long code = static_cast<unsigned long>(result);
    return win32ErrorText(code);
}

} // namespace yawa
