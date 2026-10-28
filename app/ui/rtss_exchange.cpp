#include "ui/rtss_exchange.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <vector>

namespace yawa {
namespace {
    constexpr std::size_t kMaxExchangeBytes = 8 * 1024 * 1024;
    constexpr std::size_t kMaxExpandedBytes = 24 * 1024 * 1024;

    std::uint32_t read32(std::string_view bytes, std::size_t at)
    {
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            result |= static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[at + index]))
                << (8 * index);
        }
        return result;
    }

    bool expandLzw(std::string_view compressed, std::size_t expected, std::string& decoded)
    {
        std::array<std::string, 4096> dictionary { };
        for (int code = 0; code < 256; ++code) {
            dictionary[static_cast<std::size_t>(code)] = static_cast<char>(code);
        }
        decoded.clear();
        decoded.reserve(expected);
        std::string previous;
        std::size_t bitOffset = 0;
        int nextCode = 258;
        int codeBits = 9;
        bool started = false;
        while (bitOffset + static_cast<std::size_t>(codeBits) <= compressed.size() * 8) {
            int code = 0;
            for (int bit = 0; bit < codeBits; ++bit) {
                const std::size_t at = bitOffset + static_cast<std::size_t>(bit);
                const auto byte = static_cast<unsigned char>(compressed[at / 8]);
                code |= ((byte >> (at % 8)) & 1u) << bit;
            }
            bitOffset += static_cast<std::size_t>(codeBits);
            if (code == 256) {
                nextCode = 258;
                codeBits = 9;
                previous.clear();
                started = true;
                continue;
            }
            if (!started) {
                return false;
            }
            if (code == 257) {
                if (decoded.size() != expected) {
                    return false;
                }
                for (; bitOffset < compressed.size() * 8; ++bitOffset) {
                    if ((static_cast<unsigned char>(compressed[bitOffset / 8]) >> (bitOffset % 8)) & 1u) {
                        return false;
                    }
                }
                return true;
            }
            if (code >= nextCode || (code >= 256 && code < 258)) {
                if (code != nextCode || previous.empty()) {
                    return false;
                }
            }
            const std::string value = code == nextCode ? previous + previous.front()
                : code < 256                           ? std::string(1, static_cast<char>(code))
                                                       : dictionary[static_cast<std::size_t>(code)];
            if (value.empty() || value.size() > expected - decoded.size()) {
                return false;
            }
            decoded += value;
            if (!previous.empty() && nextCode < 4096) {
                dictionary[static_cast<std::size_t>(nextCode++)] = previous + value.front();
                if (nextCode == (1 << codeBits) && codeBits < 12) {
                    ++codeBits;
                }
            }
            previous = value;
        }
        return false;
    }

    bool isLayoutName(std::string_view name)
    {
        if (name.size() < 5 || name.size() > 256 || name.find_first_of("/\\:") != std::string_view::npos
            || name == "." || name == ".." || name.find("..") != std::string_view::npos) {
            return false;
        }
        std::string extension(name.substr(name.size() - 4));
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return extension == ".ovl";
    }

    bool safeFileName(std::string_view name)
    {
        return !name.empty() && name.size() <= 256 && name != "." && name != ".."
            && name.find("..") == std::string_view::npos
            && name.find_first_of("/\\:") == std::string_view::npos;
    }
}

bool unpackRtssExchange(std::string_view bytes, RtssExchange& exchange, std::string& error)
{
    error.clear();
    if (bytes.size() < 26 || bytes.size() > kMaxExchangeBytes || bytes.substr(0, 4) != "0CDU"
        || bytes.substr(20, 4) != "0WZL") {
        error = "Неподдерживаемый или слишком большой пакет RTSS .ovx";
        return false;
    }
    const std::size_t expanded = read32(bytes, 4);
    if (expanded < 22 || expanded > kMaxExpandedBytes || read32(bytes, 8) != bytes.size() - 24) {
        error = "Повреждённые размеры пакета RTSS .ovx";
        return false;
    }
    std::string archive;
    if (!expandLzw(bytes.substr(24), expanded, archive) || archive.substr(0, 4) != "0AZS"
        || read32(archive, 4) != archive.size()) {
        error = "Не удалось распаковать пакет RTSS .ovx";
        return false;
    }
    const std::uint32_t files = read32(archive, 8);
    if (files == 0 || files > 64) {
        error = "Недопустимое число файлов в пакете RTSS .ovx";
        return false;
    }
    RtssExchange parsed;
    std::vector<std::string_view> names;
    bool foundLayout = false;
    std::size_t at = 12;
    for (std::uint32_t index = 0; index < files; ++index) {
        if (at > archive.size() || archive.size() - at < 8) {
            error = "Неполный список файлов RTSS .ovx";
            return false;
        }
        const std::size_t entrySize = read32(archive, at);
        const std::size_t dataSize = read32(archive, at + 4);
        at += 8;
        if (entrySize > archive.size() - at || dataSize > entrySize || entrySize - dataSize < 2
            || entrySize - dataSize > 257) {
            error = "Повреждённый файл внутри RTSS .ovx";
            return false;
        }
        const std::string_view name(archive.data() + at + dataSize, entrySize - dataSize - 1);
        if (archive[at + entrySize - 1] != '\0' || name.find('\0') != std::string_view::npos
            || !safeFileName(name) || std::find(names.begin(), names.end(), name) != names.end()) {
            error = "Недопустимое имя файла внутри RTSS .ovx";
            return false;
        }
        names.push_back(name);
        if (isLayoutName(name)) {
            if (foundLayout || dataSize > 1024 * 1024) {
                error = "Неоднозначный или слишком большой макет RTSS .ovl в пакете";
                return false;
            }
            foundLayout = true;
            parsed.layout.assign(archive.data() + at, dataSize);
        } else {
            ++parsed.resources;
        }
        at += entrySize;
    }
    if (at != archive.size() || parsed.layout.empty()) {
        error = "В пакете RTSS .ovx нет одного текстового макета .ovl";
        return false;
    }
    exchange = std::move(parsed);
    return true;
}

} // namespace yawa
