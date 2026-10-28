#include "report/package.h"
#include "report/package_internal.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yawa {
namespace report_internal {

    constexpr std::string_view kPngSignature = "\x89PNG\r\n\x1a\n";

    std::uint32_t crc32(std::string_view data)
    {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (unsigned char byte : data) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
        return ~crc;
    }

    std::uint32_t bigEndian(std::string_view data, std::size_t offset)
    {
        return (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset])) << 24)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 1])) << 16)
            | (static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + 2])) << 8)
            | static_cast<unsigned char>(data[offset + 3]);
    }

    bool validPng(std::string_view png)
    {
        if (png.substr(0, kPngSignature.size()) != kPngSignature)
            return false;
        std::size_t offset = kPngSignature.size();
        bool hasImage = false;
        bool hasPixels = false;
        while (offset + 12 <= png.size()) {
            const auto length = bigEndian(png, offset);
            if (length > png.size() - offset - 12)
                return false;
            const auto type = png.substr(offset + 4, 4);
            if (!hasImage) {
                if (type != "IHDR" || length != 13)
                    return false;
                const auto width = bigEndian(png, offset + 8);
                const auto height = bigEndian(png, offset + 12);
                if (width == 0 || width > 8192 || height == 0 || height > 8192
                    || static_cast<std::uint64_t>(width) * height > 16u * 1024u * 1024u)
                    return false;
                hasImage = true;
            } else if (type == "IHDR") {
                return false;
            }
            const auto chunk = png.substr(offset + 4, 4 + length);
            if (crc32(chunk) != bigEndian(png, offset + 8 + length))
                return false;
            offset += 12 + length;
            if (type == "IDAT")
                hasPixels = true;
            if (type == "IEND")
                return length == 0 && hasPixels && offset == png.size();
        }
        return false;
    }

} // namespace report_internal
namespace {

    using report_internal::crc32;
    using report_internal::validPng;

    constexpr std::size_t kMaxCardBytes = 32u * 1024u * 1024u;
    void word(std::string& output, std::uint16_t value)
    {
        output.push_back(static_cast<char>(value));
        output.push_back(static_cast<char>(value >> 8));
    }

    void dword(std::string& output, std::uint32_t value)
    {
        word(output, static_cast<std::uint16_t>(value));
        word(output, static_cast<std::uint16_t>(value >> 16));
    }

    void jsonString(std::string& output, std::string_view text)
    {
        constexpr char digits[] = "0123456789abcdef";
        output.push_back('"');
        for (unsigned char byte : text) {
            switch (byte) {
            case '"':
                output += "\\\"";
                break;
            case '\\':
                output += "\\\\";
                break;
            case '\n':
                output += "\\n";
                break;
            case '\r':
                output += "\\r";
                break;
            case '\t':
                output += "\\t";
                break;
            default:
                if (byte < 0x20u) {
                    output += "\\u00";
                    output.push_back(digits[byte >> 4]);
                    output.push_back(digits[byte & 0xFu]);
                } else {
                    output.push_back(static_cast<char>(byte));
                }
            }
        }
        output.push_back('"');
    }

    void optionalPercent(std::string& output, std::string_view key, std::optional<double> value)
    {
        if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 100.0)
            return;
        output.push_back(',');
        jsonString(output, key);
        output += ':' + std::to_string(*value);
    }

    std::string sessionJson(const GameDiagnosisInput& input)
    {
        std::string output = "{\"status\":\"no_game_sample\"";
        if (input.gameProcessId != 0 && input.capture == GameCaptureState::Running
            && input.nowMs >= input.sampledAtMs && input.nowMs - input.sampledAtMs <= 2000) {
            output = "{\"status\":\"instantaneous_only\",\"sample_age_ms\":"
                + std::to_string(input.nowMs - input.sampledAtMs);
            if (input.fps && std::isfinite(*input.fps) && *input.fps > 0.0 && *input.fps <= 100000.0)
                output += ",\"fps\":" + std::to_string(*input.fps);
            optionalPercent(output, "game_cpu_pct", input.gameCpuLoad);
            optionalPercent(output, "game_thread_pct", input.gameThreadLoad);
            if (input.gpuMatchesGame)
                optionalPercent(output, "game_gpu_3d_pct", input.gpuLoad);
        }
        return output + "}\n";
    }

    std::string diagnosisJson(const GameDiagnosisInput& input)
    {
        std::string output = "{\"findings\":[";
        bool first = true;
        for (const Finding& finding : diagnoseGame(input)) {
            if (!first)
                output.push_back(',');
            first = false;
            output += "{\"code\":";
            jsonString(output, finding.code);
            output += ",\"severity\":";
            constexpr std::array<std::string_view, 4> names { "info", "advice", "warning", "critical" };
            jsonString(output, names[std::min(static_cast<std::size_t>(finding.severity), names.size() - 1)]);
            output += ",\"title\":";
            jsonString(output, finding.title);
            output += ",\"detail\":";
            jsonString(output, finding.detail);
            output += ",\"action\":";
            jsonString(output, finding.action);
            output += ",\"confidence\":" + std::to_string(finding.confidence) + '}';
        }
        return output + "]}\n";
    }

    struct Entry
    {
        std::string_view name;
        std::uint32_t offset;
        std::uint32_t size;
        std::uint32_t checksum;
    };

    void addEntry(
        std::string& output, std::vector<Entry>& entries, std::string_view name, std::string_view contents)
    {
        const auto offset = static_cast<std::uint32_t>(output.size());
        const auto size = static_cast<std::uint32_t>(contents.size());
        const auto checksum = crc32(contents);
        dword(output, 0x04034B50u);
        word(output, 20);
        word(output, 0);
        word(output, 0); // STORE; deterministic bytes and no runtime codec.
        word(output, 0);
        word(output, 0);
        dword(output, checksum);
        dword(output, size);
        dword(output, size);
        word(output, static_cast<std::uint16_t>(name.size()));
        word(output, 0);
        output.append(name);
        output.append(contents);
        entries.push_back({ name, offset, size, checksum });
    }

    void finishZip(std::string& output, const std::vector<Entry>& entries)
    {
        const auto start = static_cast<std::uint32_t>(output.size());
        for (const Entry& entry : entries) {
            dword(output, 0x02014B50u);
            word(output, 20);
            word(output, 20);
            word(output, 0);
            word(output, 0);
            word(output, 0);
            word(output, 0);
            dword(output, entry.checksum);
            dword(output, entry.size);
            dword(output, entry.size);
            word(output, static_cast<std::uint16_t>(entry.name.size()));
            word(output, 0);
            word(output, 0);
            word(output, 0);
            word(output, 0);
            dword(output, 0);
            dword(output, entry.offset);
            output.append(entry.name);
        }
        const auto end = static_cast<std::uint32_t>(output.size());
        dword(output, 0x06054B50u);
        word(output, 0);
        word(output, 0);
        word(output, static_cast<std::uint16_t>(entries.size()));
        word(output, static_cast<std::uint16_t>(entries.size()));
        dword(output, end - start);
        dword(output, start);
        word(output, 0);
    }

} // namespace

bool buildReportPackage(const ReportPackageInput& input, std::string& package, std::string& error)
{
    error.clear();
    if (input.cardPng.size() > kMaxCardBytes) {
        error = "Отчёт: PNG-карточка больше 32 МиБ";
        return false;
    }
    if (!validPng(input.cardPng)) {
        error = "Отчёт: отсутствует или повреждена PNG-карточка для предпросмотра";
        return false;
    }
    std::string output;
    std::vector<Entry> entries;
    const std::string manifest = std::string("{\"schema\":1,\"format\":\"yawa\",\"game_frame\":")
        + (input.gameFrameIncluded ? "\"included\"" : "\"omitted\"") + "}\n";
    addEntry(output, entries, "manifest.json", manifest);
    addEntry(output, entries, "system.json", "{\"status\":\"not_collected\"}\n");
    addEntry(output, entries, "session.json", sessionJson(input.diagnosis));
    addEntry(output, entries, "sensors.csv", "metric,value,unit\n");
    addEntry(output, entries, "audit.json", "{\"status\":\"not_collected\"}\n");
    addEntry(output, entries, "diagnosis.json", diagnosisJson(input.diagnosis));
    addEntry(output, entries, "screen.png", input.cardPng);
    finishZip(output, entries);
    package = std::move(output);
    return true;
}

} // namespace yawa
