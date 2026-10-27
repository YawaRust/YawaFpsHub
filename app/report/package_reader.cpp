#include "report/package.h"
#include "report/package_internal.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace yawa {
namespace {

    constexpr std::array<std::string_view, 7> kNames { "manifest.json", "system.json", "session.json",
        "sensors.csv", "audit.json", "diagnosis.json", "screen.png" };
    constexpr std::size_t kMaxArchiveBytes = 33u * 1024u * 1024u;
    constexpr std::size_t kMaxMetadataBytes = 64u * 1024u;
    constexpr std::size_t kMaxCardBytes = 32u * 1024u * 1024u;

    std::uint16_t read16(std::string_view bytes, std::size_t at)
    {
        return static_cast<unsigned char>(bytes[at])
            | static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at + 1])) << 8;
    }

    std::uint32_t read32(std::string_view bytes, std::size_t at)
    {
        return read16(bytes, at) | static_cast<std::uint32_t>(read16(bytes, at + 2)) << 16;
    }

    class ManifestReader
    {
    public:
        explicit ManifestReader(std::string_view text)
            : m_text(text)
        {
        }

        bool take(char symbol)
        {
            space();
            if (m_pos == m_text.size() || m_text[m_pos] != symbol)
                return false;
            ++m_pos;
            return true;
        }

        bool quoted(std::string_view& result)
        {
            if (!take('"'))
                return false;
            const std::size_t start = m_pos;
            while (m_pos < m_text.size() && m_text[m_pos] != '"') {
                const unsigned char ch = static_cast<unsigned char>(m_text[m_pos++]);
                if (ch < 0x20 || ch > 0x7e || ch == '\\')
                    return false;
            }
            if (m_pos == m_text.size())
                return false;
            result = m_text.substr(start, m_pos++ - start);
            return true;
        }

        bool ended()
        {
            space();
            return m_pos == m_text.size();
        }

    private:
        void space()
        {
            while (m_pos < m_text.size()
                && (m_text[m_pos] == ' ' || m_text[m_pos] == '\r' || m_text[m_pos] == '\n'
                    || m_text[m_pos] == '\t'))
                ++m_pos;
        }
        std::string_view m_text;
        std::size_t m_pos = 0;
    };

    bool parseManifest(std::string_view json, bool& gameFrameIncluded)
    {
        ManifestReader reader(json);
        if (!reader.take('{'))
            return false;
        bool schema = false;
        bool format = false;
        bool frame = false;
        bool first = true;
        while (true) {
            if (reader.take('}'))
                break;
            if (!first && !reader.take(','))
                return false;
            first = false;
            std::string_view key;
            if (!reader.quoted(key) || !reader.take(':'))
                return false;
            if (key == "schema") {
                if (schema || !reader.take('1'))
                    return false;
                schema = true;
            } else {
                std::string_view value;
                if (!reader.quoted(value))
                    return false;
                if (key == "format" && !format && value == "yawa") {
                    format = true;
                } else if (key == "game_frame" && !frame && (value == "included" || value == "omitted")) {
                    gameFrameIncluded = value == "included";
                    frame = true;
                } else {
                    return false;
                }
            }
        }
        return schema && format && frame && reader.ended();
    }

    bool readEntries(std::string_view archive, std::array<std::string_view, 7>& entries, std::string& error)
    {
        if (archive.size() < 22 || archive.size() > kMaxArchiveBytes) {
            error = "Пакет .yawa пуст или превышает 33 МиБ";
            return false;
        }
        const auto footer = archive.size() - 22;
        const auto central = read32(archive, footer + 16);
        const auto centralBytes = read32(archive, footer + 12);
        if (read32(archive, footer) != 0x06054B50u || read16(archive, footer + 4) != 0
            || read16(archive, footer + 6) != 0 || read16(archive, footer + 8) != kNames.size()
            || read16(archive, footer + 10) != kNames.size() || read16(archive, footer + 20) != 0
            || central > footer || centralBytes != footer - central) {
            error = "Пакет .yawa: неверный каталог ZIP";
            return false;
        }
        std::size_t directory = central;
        std::size_t local = 0;
        for (std::size_t index = 0; index < kNames.size(); ++index) {
            if (directory + 46 > footer || read32(archive, directory) != 0x02014B50u) {
                error = "Пакет .yawa: повреждён каталог ZIP";
                return false;
            }
            const auto nameBytes = read16(archive, directory + 28);
            const auto bytes = read32(archive, directory + 24);
            const auto storedBytes = read32(archive, directory + 20);
            const auto checksum = read32(archive, directory + 16);
            const auto name = kNames[index];
            if (nameBytes != name.size() || read16(archive, directory + 8) != 0
                || read16(archive, directory + 10) != 0 || storedBytes != bytes
                || read16(archive, directory + 30) != 0 || read16(archive, directory + 32) != 0
                || read16(archive, directory + 34) != 0 || read32(archive, directory + 42) != local
                || directory + 46 + nameBytes > footer || archive.substr(directory + 46, nameBytes) != name
                || bytes > (index == 6 ? kMaxCardBytes : kMaxMetadataBytes)) {
                error = "Пакет .yawa: неверная запись " + std::string(name);
                return false;
            }
            directory += 46 + nameBytes;
            if (local + 30 > central || read32(archive, local) != 0x04034B50u
                || read16(archive, local + 6) != 0 || read16(archive, local + 8) != 0
                || read32(archive, local + 14) != checksum || read32(archive, local + 18) != storedBytes
                || read32(archive, local + 22) != bytes || read16(archive, local + 26) != nameBytes
                || read16(archive, local + 28) != 0 || local + 30 + nameBytes > central
                || archive.substr(local + 30, nameBytes) != name) {
                error = "Пакет .yawa: повреждён файл " + std::string(name);
                return false;
            }
            const auto data = local + 30 + nameBytes;
            if (bytes > central - data) {
                error = "Пакет .yawa: файл выходит за границы архива";
                return false;
            }
            entries[index] = archive.substr(data, bytes);
            if (report_internal::crc32(entries[index]) != checksum) {
                error = "Пакет .yawa: ошибка CRC файла " + std::string(name);
                return false;
            }
            local = data + bytes;
        }
        if (directory != footer || local != central) {
            error = "Пакет .yawa: лишние данные ZIP";
            return false;
        }
        return true;
    }

} // namespace

bool readReportPackage(std::string_view archive, ReportPackageContents& contents, std::string& error)
{
    error.clear();
    std::array<std::string_view, 7> entries { };
    if (!readEntries(archive, entries, error))
        return false;
    bool hasFrame = false;
    if (!parseManifest(entries[0], hasFrame) || !report_internal::validPng(entries[6])) {
        error = "Пакет .yawa: неподдерживаемая схема или повреждённая PNG-карточка";
        return false;
    }
    ReportPackageContents parsed;
    parsed.manifestJson = entries[0];
    parsed.systemJson = entries[1];
    parsed.sessionJson = entries[2];
    parsed.sensorsCsv = entries[3];
    parsed.auditJson = entries[4];
    parsed.diagnosisJson = entries[5];
    parsed.cardPng = entries[6];
    parsed.gameFrameIncluded = hasFrame;
    contents = std::move(parsed);
    return true;
}

} // namespace yawa
