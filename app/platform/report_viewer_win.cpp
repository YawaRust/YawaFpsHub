#include "platform/report_viewer_win.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>

namespace yawa {
namespace {

    using Microsoft::WRL::ComPtr;
    constexpr wchar_t kViewerClass[] = L"YawaFpsHubReportViewer";

    struct ViewerState
    {
        HWND window = nullptr;
        bool hasFrame = false;
        ReportViewedCard image;
    };

    bool decodeCard(std::string_view png, ReportViewedCard& image, std::string& error)
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapDecoder> decoder;
        ComPtr<IWICBitmapFrameDecode> frame;
        ComPtr<IWICFormatConverter> converter;
        // WIC принимает изменяемый указатель, но декодер читает буфер без записи.
        if (FAILED(CoCreateInstance(
                CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))
            || FAILED(factory->CreateStream(&stream))
            || FAILED(stream->InitializeFromMemory(
                reinterpret_cast<BYTE*>(const_cast<char*>(png.data())), static_cast<DWORD>(png.size())))
            || FAILED(factory->CreateDecoderFromStream(
                stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad, &decoder))
            || FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&image.width, &image.height))) {
            error = "Просмотр: PNG из пакета не декодируется";
            return false;
        }
        if (image.width == 0 || image.height == 0 || image.width > 8192 || image.height > 8192
            || static_cast<std::uint64_t>(image.width) * image.height > 16000000u) {
            error = "Просмотр: размеры PNG не поддерживаются";
            return false;
        }
        image.bgra.resize(static_cast<std::size_t>(image.width) * image.height * 4u);
        if (FAILED(factory->CreateFormatConverter(&converter))
            || FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppPBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))
            || FAILED(converter->CopyPixels(
                nullptr, image.width * 4u, static_cast<UINT>(image.bgra.size()), image.bgra.data()))) {
            error = "Просмотр: пиксели PNG недоступны";
            return false;
        }
        return true;
    }

    LRESULT CALLBACK viewerProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto* created = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* state = static_cast<ViewerState*>(created->lpCreateParams);
            state->window = window;
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        }
        auto* state = reinterpret_cast<ViewerState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr)
            return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = POINT { 700, 450 };
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint { };
            const HDC dc = BeginPaint(window, &paint);
            RECT client { };
            GetClientRect(window, &client);
            FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(235, 244, 245));
            const wchar_t* warning
                = L"Чужой пакет: структура проверена, но достоверность цифр и кадра не подтверждена.";
            RECT heading { 20, 12, client.right - 20, 50 };
            DrawTextW(dc, warning, -1, &heading, DT_LEFT | DT_WORDBREAK);
            const wchar_t* frame
                = state->hasFrame ? L"Кадр заявлен как включённый" : L"Кадр заявлен как отсутствующий";
            RECT footer { 20, client.bottom - 35, client.right - 20, client.bottom - 8 };
            DrawTextW(dc, frame, -1, &footer, DT_LEFT | DT_SINGLELINE);
            const int availableWidth = static_cast<int>(std::max(1L, client.right - 40));
            const int availableHeight = static_cast<int>(std::max(1L, client.bottom - 110));
            const double scale = std::min(static_cast<double>(availableWidth) / state->image.width,
                static_cast<double>(availableHeight) / state->image.height);
            const int width = std::max(1, static_cast<int>(std::lround(state->image.width * scale)));
            const int height = std::max(1, static_cast<int>(std::lround(state->image.height * scale)));
            BITMAPINFO bitmap { };
            bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmap.bmiHeader.biWidth = static_cast<LONG>(state->image.width);
            bitmap.bmiHeader.biHeight = -static_cast<LONG>(state->image.height);
            bitmap.bmiHeader.biPlanes = 1;
            bitmap.bmiHeader.biBitCount = 32;
            bitmap.bmiHeader.biCompression = BI_RGB;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, (client.right - width) / 2, 60, width, height, 0, 0,
                static_cast<int>(state->image.width), static_cast<int>(state->image.height),
                state->image.bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            state->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

} // namespace

bool decodeReportCardPng(std::string_view png, ReportViewedCard& image, std::string& error)
{
    error.clear();
    ReportViewedCard decoded;
    if (!decodeCard(png, decoded, error))
        return false;
    image = std::move(decoded);
    return true;
}

bool showReportPackageViewer(HWND owner, const ReportPackageContents& contents, std::string& error)
{
    ViewerState state;
    state.hasFrame = contents.gameFrameIncluded;
    if (!decodeReportCardPng(contents.cardPng, state.image, error))
        return false;
    WNDCLASSW cls { };
    cls.lpfnWndProc = viewerProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kViewerClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Просмотр: не удалось зарегистрировать окно";
        return false;
    }
    HWND viewer = CreateWindowExW(WS_EX_DLGMODALFRAME, kViewerClass,
        L"Просмотр локального .yawa · YawaFpsHub", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080,
        740, owner, nullptr, cls.hInstance, &state);
    if (viewer == nullptr) {
        error = "Просмотр: не удалось открыть окно";
        return false;
    }
    EnableWindow(owner, FALSE);
    ShowWindow(viewer, SW_SHOW);
    UpdateWindow(viewer);
    MSG event { };
    BOOL received = 0;
    while (state.window != nullptr && (received = GetMessageW(&event, nullptr, 0, 0)) > 0) {
        TranslateMessage(&event);
        DispatchMessageW(&event);
    }
    if (state.window != nullptr)
        DestroyWindow(state.window);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (received == 0) {
        PostQuitMessage(static_cast<int>(event.wParam));
    } else if (received == -1) {
        error = "Просмотр: ошибка обработки сообщений окна";
        return false;
    }
    return true;
}

} // namespace yawa
