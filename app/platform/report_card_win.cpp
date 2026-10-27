#include "platform/report_card_win.h"
#include "platform/win_util.h"

#include <d2d1.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <utility>

namespace yawa {
namespace {

    using Microsoft::WRL::ComPtr;
    constexpr UINT kWidth = 1920;
    constexpr UINT kHeight = 1080;

    bool format(
        IDWriteFactory* factory, float size, DWRITE_FONT_WEIGHT weight, ComPtr<IDWriteTextFormat>& result)
    {
        const HRESULT status = factory->CreateTextFormat(L"Segoe UI", nullptr, weight,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"ru-RU", &result);
        if (FAILED(status))
            return false;
        result->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        return true;
    }

    void text(ID2D1RenderTarget* canvas, ID2D1SolidColorBrush* brush, IDWriteTextFormat* font,
        std::wstring_view value, const D2D1_RECT_F& rect, const D2D1_COLOR_F& color)
    {
        brush->SetColor(color);
        canvas->DrawTextW(
            value.data(), static_cast<UINT32>(value.size()), font, rect, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    void fill(ID2D1RenderTarget* canvas, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& rect,
        const D2D1_COLOR_F& color)
    {
        brush->SetColor(color);
        canvas->FillRectangle(rect, brush);
    }

    std::wstring percent(std::optional<double> value)
    {
        if (!value || !std::isfinite(*value) || *value < 0.0 || *value > 100.0)
            return L"нет данных";
        return std::to_wstring(static_cast<int>(std::lround(*value))) + L" %";
    }

    void paint(ID2D1RenderTarget* canvas, ID2D1SolidColorBrush* brush, IDWriteTextFormat* title,
        IDWriteTextFormat* body, IDWriteTextFormat* caption, const GameDiagnosisInput& input)
    {
        const auto white = D2D1::ColorF(0.92f, 0.97f, 0.97f);
        const auto dim = D2D1::ColorF(0.70f, 0.80f, 0.84f);
        const auto accent = D2D1::ColorF(0.37f, 0.95f, 0.61f);
        canvas->BeginDraw();
        canvas->Clear(D2D1::ColorF(0.05f, 0.09f, 0.12f));
        text(canvas, brush, title, L"YawaFpsHub · диагностический снимок", D2D1::RectF(48, 30, 1850, 94),
            white);
        text(canvas, brush, caption, L"ОФЛАЙН · КАДР ИГРЫ НЕ ВКЛЮЧЁН", D2D1::RectF(48, 100, 1100, 138),
            accent);
        fill(canvas, brush, D2D1::RectF(48, 155, 1108, 840), D2D1::ColorF(0.09f, 0.15f, 0.18f));
        text(canvas, brush, body, L"Кадр не захвачен. Карточка не копирует рабочий стол и другие окна.",
            D2D1::RectF(120, 432, 1030, 544), dim);
        fill(canvas, brush, D2D1::RectF(1134, 155, 1872, 840), D2D1::ColorF(0.09f, 0.15f, 0.18f));

        const auto findings = diagnoseGame(input);
        const Finding* finding = findings.empty() ? nullptr : &findings.front();
        text(canvas, brush, caption, finding ? L"НАХОДКА · ЗАМЕР ИГРЫ" : L"НЕТ ДОСТАТОЧНЫХ ДАННЫХ",
            D2D1::RectF(1170, 186, 1820, 223), finding ? accent : dim);
        const std::wstring headline = finding ? widen(finding->title) : L"Недостаточно данных для вывода";
        text(canvas, brush, title, headline, D2D1::RectF(1170, 237, 1830, 350), white);
        const std::wstring detail = finding ? widen(finding->detail)
            : input.gameProcessId == 0
            ? L"Активная игра не обнаружена. Общая загрузка ПК не заменяет замер игры."
            : L"Нужны свежие кадры, CPU игры и GPU 3D этой игры на выбранном адаптере.";
        text(canvas, brush, body, detail, D2D1::RectF(1170, 385, 1828, 554), dim);
        text(canvas, brush, body,
            finding ? widen(finding->action) : L"Проверьте сбор кадров и показатели игры.",
            D2D1::RectF(1170, 625, 1828, 800), white);

        const bool fresh = input.gameProcessId != 0 && input.capture == GameCaptureState::Running
            && input.nowMs >= input.sampledAtMs && input.nowMs - input.sampledAtMs <= 2000;
        text(canvas, brush, caption,
            L"FPS: "
                + (fresh && input.fps && std::isfinite(*input.fps) && *input.fps > 0.0
                            && *input.fps <= 100000.0
                        ? std::to_wstring(static_cast<int>(std::lround(*input.fps)))
                        : L"нет данных"),
            D2D1::RectF(48, 878, 470, 921), white);
        text(canvas, brush, caption, L"CPU игры: " + (fresh ? percent(input.gameCpuLoad) : L"нет данных"),
            D2D1::RectF(480, 878, 900, 921), white);
        text(canvas, brush, caption,
            L"GPU 3D игры: " + (fresh && input.gpuMatchesGame ? percent(input.gpuLoad) : L"нет данных"),
            D2D1::RectF(950, 878, 1650, 921), white);
        text(canvas, brush, caption,
            L"Только проверенные показания активной игры · без серийных номеров, путей и журналов",
            D2D1::RectF(48, 977, 1840, 1030), dim);
    }

    bool encodePng(IWICImagingFactory* factory, IWICBitmap* bitmap, std::string& png)
    {
        ComPtr<IStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> frame;
        if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))
            || FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            || FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))
            || FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)))
            return false;
        WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(frame->SetSize(kWidth, kHeight)) || FAILED(frame->SetPixelFormat(&pixelFormat))
            || FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit())
            || FAILED(encoder->Commit()))
            return false;
        STATSTG info { };
        if (FAILED(stream->Stat(&info, STATFLAG_NONAME)) || info.cbSize.QuadPart > 32u * 1024u * 1024u)
            return false;
        png.resize(static_cast<std::size_t>(info.cbSize.QuadPart));
        LARGE_INTEGER start { };
        ULONG received = 0;
        return SUCCEEDED(stream->Seek(start, STREAM_SEEK_SET, nullptr))
            && SUCCEEDED(stream->Read(png.data(), static_cast<ULONG>(png.size()), &received))
            && received == static_cast<ULONG>(png.size());
    }

} // namespace

bool renderReportCard(const GameDiagnosisInput& input, ReportCardBitmap& result, std::string& error)
{
    error.clear();
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> bitmap;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<ID2D1RenderTarget> canvas;
    ComPtr<ID2D1SolidColorBrush> brush;
    ComPtr<IDWriteFactory> dwrite;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))
        || FAILED(
            wic->CreateBitmap(kWidth, kHeight, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap))
        || FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf()))
        || FAILED(d2d->CreateWicBitmapRenderTarget(bitmap.Get(), D2D1::RenderTargetProperties(), &canvas))
        || FAILED(canvas->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f), &brush))
        || FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(dwrite.GetAddressOf())))) {
        error = "Карточка: Direct2D/DirectWrite или WIC недоступны";
        return false;
    }
    ComPtr<IDWriteTextFormat> heading;
    ComPtr<IDWriteTextFormat> body;
    ComPtr<IDWriteTextFormat> caption;
    if (!format(dwrite.Get(), 45.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, heading)
        || !format(dwrite.Get(), 30.0f, DWRITE_FONT_WEIGHT_NORMAL, body)
        || !format(dwrite.Get(), 25.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, caption)) {
        error = "Карточка: не удалось создать шрифты";
        return false;
    }
    paint(canvas.Get(), brush.Get(), heading.Get(), body.Get(), caption.Get(), input);
    if (FAILED(canvas->EndDraw())) {
        error = "Карточка: не удалось завершить отрисовку";
        return false;
    }
    ReportCardBitmap card;
    card.bgra.resize(static_cast<std::size_t>(kWidth) * kHeight * 4u);
    if (FAILED(
            bitmap->CopyPixels(nullptr, kWidth * 4u, static_cast<UINT>(card.bgra.size()), card.bgra.data()))
        || !encodePng(wic.Get(), bitmap.Get(), card.png)) {
        error = "Карточка: не удалось получить PNG";
        return false;
    }
    result = std::move(card);
    return true;
}

} // namespace yawa
