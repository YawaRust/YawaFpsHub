#include "platform/report_card_win.h"
#include "platform/report_viewer_win.h"
#include "report/package.h"

#include <objbase.h>
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr unsigned kWidth = 1920;

bool pixelEquals(const yawa::ReportCardBitmap& a, const yawa::ReportCardBitmap& b, unsigned x, unsigned y)
{
    const std::size_t offset = (static_cast<std::size_t>(y) * kWidth + x) * 4u;
    for (std::size_t channel = 0; channel < 4; ++channel) {
        if (a.bgra[offset + channel] != b.bgra[offset + channel])
            return false;
    }
    return true;
}

bool check(bool condition, std::string_view description)
{
    if (!condition)
        std::cerr << description << '\n';
    return condition;
}

} // namespace

int main()
{
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized))
        return 1;

    yawa::GameDiagnosisInput input;
    input.nowMs = 10'000;
    yawa::GameClientFrame game;
    game.width = 100;
    game.height = 100;
    game.bgra.resize(static_cast<std::size_t>(game.width) * game.height * 4u);
    for (std::size_t i = 0; i < game.bgra.size(); i += 4) {
        game.bgra[i] = 255; // Bright blue in BGRA, visibly distinct from the opaque mask.
        game.bgra[i + 3] = 255;
    }
    yawa::ReportCardBitmap blank;
    yawa::ReportCardBitmap visible;
    yawa::ReportCardBitmap masked;
    std::string error;
    const yawa::ReportMask mask { 400, 300, 700, 600 };
    bool rendered = yawa::renderReportCard(input, nullptr, { }, blank, error);
    bool ok = check(rendered, "Blank card: " + error);
    rendered = yawa::renderReportCard(input, &game, { }, visible, error);
    ok = check(rendered, "Game card: " + error) && ok;
    rendered = yawa::renderReportCard(input, &game, std::span(&mask, 1), masked, error);
    ok = check(rendered, "Masked card: " + error) && ok;
    if (ok) {
        ok = check(!pixelEquals(blank, visible, 550, 450), "Game pixels must appear in the preview") && ok;
        ok = check(!pixelEquals(visible, masked, 550, 450), "Mask must replace game pixels") && ok;
        ok = check(pixelEquals(visible, masked, 300, 450), "Mask must not affect other game pixels") && ok;
        ok = check(pixelEquals(visible, masked, 1500, 450), "Mask must not affect report text") && ok;
        ok = check(masked.png.size() >= 8 && masked.png.substr(0, 8) == "\x89PNG\r\n\x1a\n",
                 "Masked PNG signature")
            && ok;
        std::string package;
        yawa::ReportPackageContents read;
        yawa::ReportViewedCard reopened;
        bool valid = yawa::buildReportPackage({ input, masked.png, true }, package, error);
        ok = check(valid, "Create masked package: " + error) && ok;
        valid = yawa::readReportPackage(package, read, error);
        ok = check(valid && read.gameFrameIncluded, "Read masked package: " + error) && ok;
        valid = yawa::decodeReportCardPng(read.cardPng, reopened, error);
        ok = check(valid, "Decode saved PNG: " + error) && ok;
        if (valid) {
            ok = check(reopened.width == 1920 && reopened.height == 1080, "Decoded card dimensions") && ok;
            ok = check(reopened.bgra == masked.bgra, "Saved and reopened card pixels must match") && ok;
        }
        const auto originalWidth = reopened.width;
        ok = check(
                 !yawa::decodeReportCardPng("not a png", reopened, error) && reopened.width == originalWidth,
                 "Invalid PNG must leave the viewer image unchanged")
            && ok;
    }
    CoUninitialize();
    return ok ? 0 : 1;
}
