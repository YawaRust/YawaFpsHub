#include "report/package.h"
#include "report/package_internal.h"
#include "test_support.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace yawa::test {
namespace {

    std::uint16_t read16(std::string_view data, std::size_t offset)
    {
        return static_cast<unsigned char>(data[offset])
            | static_cast<std::uint16_t>(static_cast<unsigned char>(data[offset + 1])) << 8;
    }

    std::uint32_t read32(std::string_view data, std::size_t offset)
    {
        return read16(data, offset) | static_cast<std::uint32_t>(read16(data, offset + 2)) << 16;
    }

    void write32(std::string& data, std::size_t offset, std::uint32_t value)
    {
        for (int index = 0; index < 4; ++index)
            data[offset + static_cast<std::size_t>(index)] = static_cast<char>(value >> (index * 8));
    }

    void writePng32(std::string& png, std::size_t offset, std::uint32_t value)
    {
        for (int index = 0; index < 4; ++index)
            png[offset + static_cast<std::size_t>(index)] = static_cast<char>(value >> ((3 - index) * 8));
    }

    void replaceEntryByte(std::string& archive, std::string_view name, std::size_t at, char byte)
    {
        const std::size_t localName = archive.find(name);
        const std::size_t centralName = archive.rfind(name);
        CHECK(localName != std::string::npos && localName != centralName);
        if (localName == std::string::npos || localName == centralName)
            return;
        const std::size_t data = localName + name.size();
        const std::size_t length = read32(archive, localName - 30 + 18);
        CHECK(at < length);
        if (at >= length)
            return;
        archive[data + at] = byte;
        const auto crc = report_internal::crc32(std::string_view(archive).substr(data, length));
        write32(archive, localName - 30 + 14, crc);
        write32(archive, centralName - 46 + 16, crc);
    }

} // namespace

void runReportTests()
{
    const unsigned char pngBytes[] { 0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d,
        0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x02, 0x00, 0x00, 0x00,
        0x90, 0x77, 0x53, 0xde, 0x00, 0x00, 0x00, 0x0c, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x60, 0x60,
        0x60, 0x00, 0x00, 0x00, 0x04, 0x00, 0x01, 0xf6, 0x17, 0x38, 0x55, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45,
        0x4e, 0x44, 0xae, 0x42, 0x60, 0x82 };
    const std::string png(reinterpret_cast<const char*>(pngBytes), sizeof(pngBytes));
    GameDiagnosisInput sample;
    sample.gameProcessId = 421;
    sample.nowMs = 10000;
    sample.sampledAtMs = 9500;
    sample.capture = GameCaptureState::Running;
    sample.fps = 120.0;
    sample.gameCpuLoad = 42.0;
    sample.gpuLoad = 99.0;
    sample.gpuMatchesGame = true;
    const ReportPackageInput input { sample, png, false };
    std::string output = "unchanged";
    std::string error;
    CHECK(!buildReportPackage({ sample, "not a png", false }, output, error));
    CHECK(!error.empty());
    CHECK(output == "unchanged");
    const std::string oversized = png + std::string(32u * 1024u * 1024u, 'x');
    CHECK(!buildReportPackage({ sample, oversized, false }, output, error));
    CHECK(error.find("32 МиБ") != std::string::npos);
    CHECK(output == "unchanged");
    std::string corrupted = png;
    corrupted[corrupted.size() - 1] ^= 1;
    CHECK(!buildReportPackage({ sample, corrupted, false }, output, error));
    CHECK(output == "unchanged");
    std::string hugeCanvas = png;
    writePng32(hugeCanvas, 16, 8192);
    writePng32(hugeCanvas, 20, 8192);
    writePng32(hugeCanvas, 29, report_internal::crc32(std::string_view(hugeCanvas).substr(12, 17)));
    CHECK(!buildReportPackage({ sample, hugeCanvas, false }, output, error));
    CHECK(output == "unchanged");
    CHECK(buildReportPackage(input, output, error));
    CHECK(error.empty());
    constexpr std::array<std::string_view, 7> names { "manifest.json", "system.json", "session.json",
        "sensors.csv", "audit.json", "diagnosis.json", "screen.png" };
    std::size_t offset = 0;
    for (std::string_view name : names) {
        if (offset + 30 > output.size()) {
            CHECK(false);
            return;
        }
        CHECK(read32(output, offset) == 0x04034b50u);
        CHECK(read16(output, offset + 8) == 0); // Stored, not deflated.
        const std::size_t length = read32(output, offset + 18);
        CHECK(read16(output, offset + 26) == name.size());
        const std::size_t body = offset + 30 + read16(output, offset + 26);
        if (body > output.size() || length > output.size() - body) {
            CHECK(false);
            return;
        }
        CHECK(output.substr(offset + 30, name.size()) == name);
        if (name == "manifest.json") {
            CHECK(output.substr(body, length).find("\"schema\":1") != std::string::npos);
            CHECK(output.substr(body, length).find("\"game_frame\":\"omitted\"") != std::string::npos);
        } else if (name == "session.json") {
            CHECK(output.substr(body, length).find("\"fps\":120") != std::string::npos);
            CHECK(output.substr(body, length).find("\"game_gpu_3d_pct\":99") != std::string::npos);
        } else if (name == "diagnosis.json") {
            CHECK(output.substr(body, length).find("\"code\":\"gpu_bound\"") != std::string::npos);
        } else if (name == "screen.png") {
            CHECK(output.substr(body, length) == png);
        }
        offset = body + length;
    }
    if (offset + 4 > output.size() || output.size() < 22) {
        CHECK(false);
        return;
    }
    CHECK(read32(output, offset) == 0x02014b50u);
    const std::size_t end = output.size() - 22;
    CHECK(read32(output, end) == 0x06054b50u);
    CHECK(read16(output, end + 10) == names.size());
    CHECK(read32(output, end + 16) == offset);
    CHECK(output.find("PRIVATE_USER") == std::string::npos);

    ReportPackageContents read;
    CHECK(readReportPackage(output, read, error));
    CHECK(error.empty());
    CHECK(read.cardPng == png);
    CHECK(read.sessionJson.find("\"fps\":120") != std::string::npos);
    CHECK(!read.gameFrameIncluded);
    const std::string original = read.cardPng;
    std::string broken = output;
    broken[30] = 'x'; // имя первой записи нельзя переименовать даже внутри ZIP.
    CHECK(!readReportPackage(broken, read, error));
    CHECK(read.cardPng == original);
    broken = output;
    broken[broken.find("screen.png") + 30] ^= 1; // ошибка CRC содержимого.
    CHECK(!readReportPackage(broken, read, error));
    CHECK(read.cardPng == original);
    broken = output;
    broken[8] = 8; // сжатый ZIP не поддерживается.
    CHECK(!readReportPackage(broken, read, error));
    broken = output;
    broken.back() = 1; // комментарий/данные после каталога запрещены.
    CHECK(!readReportPackage(broken, read, error));
    broken = output;
    const auto schemaValue = broken.find("\"schema\":1") + sizeof("\"schema\":") - 1;
    broken[schemaValue] = '2';
    const std::size_t manifestStart = 30 + std::string_view("manifest.json").size();
    const auto manifestCrc
        = report_internal::crc32(std::string_view(broken).substr(manifestStart, read32(broken, 18)));
    write32(broken, 14, manifestCrc);
    write32(broken, read32(broken, end + 16) + 16, manifestCrc);
    CHECK(!readReportPackage(broken, read, error)); // CRC верен, версия всё равно неизвестна.
    CHECK(read.cardPng == original);
    broken = output;
    const auto localScreenName = broken.find("screen.png");
    const auto cardStart = localScreenName + std::string_view("screen.png").size();
    broken[cardStart + 20] ^= 1;
    const auto cardCrc = report_internal::crc32(std::string_view(broken).substr(cardStart, png.size()));
    write32(broken, localScreenName - 30 + 14, cardCrc);
    write32(broken, broken.rfind("screen.png") - 46 + 16, cardCrc);
    CHECK(!readReportPackage(broken, read, error)); // ZIP CRC верен, PNG CRC неверен.
    CHECK(read.cardPng == original);
    broken = output;
    writePng32(broken, cardStart + 16, 8192);
    writePng32(broken, cardStart + 20, 8192);
    writePng32(
        broken, cardStart + 29, report_internal::crc32(std::string_view(broken).substr(cardStart + 12, 17)));
    const auto forgedCrc = report_internal::crc32(std::string_view(broken).substr(cardStart, png.size()));
    write32(broken, localScreenName - 30 + 14, forgedCrc);
    write32(broken, broken.rfind("screen.png") - 46 + 16, forgedCrc);
    CHECK(!readReportPackage(broken, read, error)); // Оба CRC верны, но площадь кадра чрезмерна.
    CHECK(read.cardPng == original);
    CHECK(!readReportPackage("not a zip", read, error));
    broken = output;
    replaceEntryByte(broken, "sensors.csv", 0, 'x');
    CHECK(!readReportPackage(broken, read, error)); // Корректный ZIP CRC не оправдывает чужой формат CSV.
    CHECK(read.cardPng == original);

    for (std::string_view name : { "system.json", "session.json", "audit.json", "diagnosis.json" }) {
        broken = output;
        replaceEntryByte(broken, name, 0, '[');
        CHECK(!readReportPackage(broken, read, error));
        CHECK(read.cardPng == original);
    }
    broken = output;
    replaceEntryByte(broken, "session.json", 11, static_cast<char>(0x80));
    CHECK(!readReportPackage(broken, read, error)); // Корректный ZIP CRC не оправдывает сломанный UTF-8.
    CHECK(read.cardPng == original);
    broken = output;
    replaceEntryByte(broken, "session.json", 12, '\\');
    CHECK(!readReportPackage(broken, read, error)); // Неизвестная управляющая последовательность.
    CHECK(read.cardPng == original);
    broken = output;
    const std::size_t sessionName = broken.find("session.json");
    const std::size_t sessionSize = read32(broken, sessionName - 30 + 18);
    replaceEntryByte(broken, "session.json", sessionSize - 2, ',');
    CHECK(!readReportPackage(broken, read, error)); // Нет закрывающей скобки JSON.
    CHECK(read.cardPng == original);

    sample.gpuMatchesGame = false;
    sample.sampledAtMs = 100;
    CHECK(buildReportPackage({ sample, png, true }, output, error));
    CHECK(output.find("\"game_frame\":\"included\"") != std::string::npos);
    CHECK(readReportPackage(output, read, error));
    CHECK(read.gameFrameIncluded);
    CHECK(output.find("\"status\":\"no_game_sample\"") != std::string::npos);
    CHECK(output.find("game_gpu_3d_pct") == std::string::npos);
    CHECK(output.find("gpu_bound") == std::string::npos);
}

} // namespace yawa::test
