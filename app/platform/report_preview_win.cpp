#include "platform/report_preview_win.h"
#include "platform/report_capture_win.h"
#include "platform/report_card_win.h"
#include "platform/win_util.h"
#include "report/package.h"

#include <commdlg.h>
#include <windows.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace yawa {
namespace {

    constexpr wchar_t kPreviewClass[] = L"YawaFpsHubReportPreview";
    constexpr int kSavePng = 1001;
    constexpr int kSavePackage = 1002;
    constexpr int kCancel = 1003;
    constexpr int kCapture = 1004;
    constexpr int kRemoveFrame = 1005;
    constexpr UINT_PTR kCaptureTimer = 1;

    struct PreviewState
    {
        HWND window = nullptr;
        HWND savePng = nullptr;
        HWND savePackage = nullptr;
        HWND cancel = nullptr;
        HWND capture = nullptr;
        HWND removeFrame = nullptr;
        GameDiagnosisInput input;
        HWND gameWindow = nullptr;
        ReportCardBitmap card;
        std::optional<GameClientFrame> frame;
        std::vector<ReportMask> masks;
        POINT maskStart { };
        bool dragging = false;
    };

    bool writeFile(const wchar_t* path, std::string_view contents)
    {
        wchar_t directory[MAX_PATH] { };
        wchar_t* basename = nullptr;
        const DWORD pathLength = GetFullPathNameW(path, MAX_PATH, directory, &basename);
        if (pathLength == 0 || pathLength >= MAX_PATH || basename == nullptr)
            return false;
        *basename = L'\0';
        wchar_t temporary[MAX_PATH] { };
        if (GetTempFileNameW(directory, L"YWA", 0, temporary) == 0)
            return false;
        HANDLE file = CreateFileW(
            temporary, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            DeleteFileW(temporary);
            return false;
        }
        DWORD written = 0;
        const bool complete
            = WriteFile(file, contents.data(), static_cast<DWORD>(contents.size()), &written, nullptr)
                != FALSE
            && written == static_cast<DWORD>(contents.size()) && FlushFileBuffers(file) != FALSE;
        const bool closed = CloseHandle(file) != FALSE;
        const bool moved = complete && closed
            && MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        if (!moved)
            DeleteFileW(temporary);
        return moved;
    }

    void save(PreviewState& state, bool package)
    {
        wchar_t filename[MAX_PATH] { };
        wcscpy_s(filename, package ? L"yawa-diagnosis.yawa" : L"yawa-diagnosis.png");
        OPENFILENAMEW dialog { };
        dialog.lStructSize = sizeof(dialog);
        dialog.hwndOwner = state.window;
        dialog.lpstrFilter = package ? L"Пакет Yawa (*.yawa)\0*.yawa\0\0" : L"PNG (*.png)\0*.png\0\0";
        dialog.lpstrDefExt = package ? L"yawa" : L"png";
        dialog.lpstrFile = filename;
        dialog.nMaxFile = MAX_PATH;
        dialog.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (GetSaveFileNameW(&dialog) == FALSE)
            return;
        std::string archive;
        std::string error;
        if (package
            && !buildReportPackage(
                { state.input, state.card.png, state.frame.has_value() }, archive, error)) {
            MessageBoxW(state.window, widen(error).c_str(), L"Пакет .yawa", MB_OK | MB_ICONERROR);
            return;
        }
        const std::string& content = package ? archive : state.card.png;
        if (!writeFile(filename, content)) {
            MessageBoxW(state.window, L"Не удалось записать выбранный файл.", L"Диагностическая карточка",
                MB_OK | MB_ICONERROR);
            return;
        }
        DestroyWindow(state.window);
    }

    void removeFrame(PreviewState& state)
    {
        ReportCardBitmap card;
        std::string error;
        if (!renderReportCard(state.input, nullptr, { }, card, error)) {
            MessageBoxW(
                state.window, widen(error).c_str(), L"Диагностическая карточка", MB_OK | MB_ICONERROR);
            return;
        }
        state.frame.reset();
        state.masks.clear();
        state.card = std::move(card);
        EnableWindow(state.removeFrame, FALSE);
        InvalidateRect(state.window, nullptr, FALSE);
    }

    void finishCapture(PreviewState& state)
    {
        KillTimer(state.window, kCaptureTimer);
        GameClientFrame captured;
        std::string error;
        const bool expectedWindow = state.gameWindow != nullptr && GetForegroundWindow() == state.gameWindow;
        if (!expectedWindow)
            error = "Захват: вернитесь в то же окно игры, которое было выбрано перед снимком";
        bool ready
            = expectedWindow && captureForegroundGameClient(state.input.gameProcessId, captured, error);
        if (ready) {
            state.input.nowMs
                = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                        .count());
            ReportCardBitmap card;
            ready = renderReportCard(state.input, &captured, { }, card, error);
            if (ready) {
                state.frame = std::move(captured);
                state.masks.clear();
                state.card = std::move(card);
                EnableWindow(state.removeFrame, TRUE);
            }
        }
        ShowWindow(state.window, SW_SHOW);
        SetForegroundWindow(state.window);
        InvalidateRect(state.window, nullptr, FALSE);
        if (!ready)
            MessageBoxW(state.window, widen(error).c_str(), L"Кадр не добавлен", MB_OK | MB_ICONWARNING);
    }

    POINT cardPoint(HWND window, int x, int y)
    {
        RECT client { };
        GetClientRect(window, &client);
        const int availableWidth = static_cast<int>(std::max(1L, client.right - 40));
        const int availableHeight = static_cast<int>(std::max(1L, client.bottom - 95));
        const double scale = std::min(
            static_cast<double>(availableWidth) / 1920.0, static_cast<double>(availableHeight) / 1080.0);
        const int width = std::max(1, static_cast<int>(std::lround(1920.0 * scale)));
        const int height = std::max(1, static_cast<int>(std::lround(1080.0 * scale)));
        return POINT { (x - (client.right - width) / 2) * 1920 / width, (y - 16) * 1080 / height };
    }

    void finishMask(PreviewState& state, POINT end)
    {
        const auto mask = reportMaskFromDrag(state.maskStart.x, state.maskStart.y, end.x, end.y);
        if (!mask || state.masks.size() == 16)
            return;
        auto masks = state.masks;
        masks.push_back(*mask);
        ReportCardBitmap card;
        std::string error;
        if (!renderReportCard(state.input, &*state.frame, masks, card, error)) {
            MessageBoxW(state.window, widen(error).c_str(), L"Маска кадра", MB_OK | MB_ICONERROR);
            return;
        }
        state.masks = std::move(masks);
        state.card = std::move(card);
        InvalidateRect(state.window, nullptr, FALSE);
    }

    void draw(HWND window, const PreviewState& state)
    {
        PAINTSTRUCT paint { };
        const HDC dc = BeginPaint(window, &paint);
        RECT client { };
        GetClientRect(window, &client);
        FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        const int availableWidth = static_cast<int>(std::max(1L, client.right - 40));
        const int availableHeight = static_cast<int>(std::max(1L, client.bottom - 95));
        const double scale = std::min(
            static_cast<double>(availableWidth) / 1920.0, static_cast<double>(availableHeight) / 1080.0);
        const int width = static_cast<int>(std::lround(1920.0 * scale));
        const int height = static_cast<int>(std::lround(1080.0 * scale));
        BITMAPINFO bitmap { };
        bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmap.bmiHeader.biWidth = 1920;
        bitmap.bmiHeader.biHeight = -1080;
        bitmap.bmiHeader.biPlanes = 1;
        bitmap.bmiHeader.biBitCount = 32;
        bitmap.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, nullptr);
        StretchDIBits(dc, (client.right - width) / 2, 16, width, height, 0, 0, 1920, 1080,
            state.card.bgra.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(window, &paint);
    }

    void placeButtons(const PreviewState& state)
    {
        RECT client { };
        GetClientRect(state.window, &client);
        const int y = client.bottom - 58;
        SetWindowPos(state.capture, nullptr, 20, y, 205, 34, SWP_NOZORDER);
        SetWindowPos(state.removeFrame, nullptr, 230, y, 125, 34, SWP_NOZORDER);
        SetWindowPos(state.savePng, nullptr, 365, y, 135, 34, SWP_NOZORDER);
        SetWindowPos(state.savePackage, nullptr, 510, y, 165, 34, SWP_NOZORDER);
        SetWindowPos(state.cancel, nullptr, client.right - 145, y, 125, 34, SWP_NOZORDER);
    }

    LRESULT CALLBACK previewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto* created = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            auto* state = static_cast<PreviewState*>(created->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            state->window = window;
        }
        auto* state = reinterpret_cast<PreviewState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (state == nullptr)
            return DefWindowProcW(window, message, wParam, lParam);
        switch (message) {
        case WM_CREATE:
            state->savePng = CreateWindowW(L"BUTTON", L"Сохранить PNG", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSavePng)),
                GetModuleHandleW(nullptr), nullptr);
            state->savePackage
                = CreateWindowW(L"BUTTON", L"Сохранить .yawa", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 0, 0, 0,
                    0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kSavePackage)),
                    GetModuleHandleW(nullptr), nullptr);
            state->cancel = CreateWindowW(L"BUTTON", L"Отмена", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 0, 0,
                0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCancel)),
                GetModuleHandleW(nullptr), nullptr);
            state->capture = CreateWindowW(L"BUTTON", L"Кадр через 5 с",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kCapture)), GetModuleHandleW(nullptr), nullptr);
            state->removeFrame
                = CreateWindowW(L"BUTTON", L"Убрать кадр", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON, 0, 0, 0, 0,
                    window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kRemoveFrame)),
                    GetModuleHandleW(nullptr), nullptr);
            if (!state->savePng || !state->savePackage || !state->cancel || !state->capture
                || !state->removeFrame)
                return -1;
            EnableWindow(state->capture, state->input.gameProcessId != 0 && state->gameWindow != nullptr);
            EnableWindow(state->removeFrame, FALSE);
            placeButtons(*state);
            return 0;
        case WM_SIZE:
            placeButtons(*state);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_PAINT:
            draw(window, *state);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lParam);
            limits->ptMinTrackSize = POINT { 850, 500 };
            return 0;
        }
        case WM_TIMER:
            if (wParam == kCaptureTimer) {
                finishCapture(*state);
                return 0;
            }
            break;
        case WM_LBUTTONDOWN:
            if (state->frame) {
                const POINT start = cardPoint(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                if (start.x >= 48 && start.x < 1108 && start.y >= 155 && start.y < 840) {
                    state->maskStart = start;
                    state->dragging = true;
                    SetCapture(window);
                }
            }
            return 0;
        case WM_LBUTTONUP:
            if (state->dragging) {
                const POINT end = cardPoint(window, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                state->dragging = false;
                ReleaseCapture();
                finishMask(*state, end);
            }
            return 0;
        case WM_CAPTURECHANGED:
            state->dragging = false;
            return 0;
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
            case kCapture:
                if (state->input.gameProcessId == 0 || state->gameWindow == nullptr)
                    return 0;
                if (MessageBoxW(window,
                        L"После подтверждения переключитесь в выбранную игру за 5 секунд. "
                        L"Кадр появится только в предпросмотре: проверьте чужие окна, "
                        L"закройте личное перетаскиванием мыши или уберите кадр.",
                        L"Добавить кадр игры", MB_OKCANCEL | MB_ICONINFORMATION)
                    != IDOK)
                    return 0;
                if (state->frame) {
                    removeFrame(*state);
                    if (state->frame)
                        return 0;
                }
                if (SetTimer(window, kCaptureTimer, 5000, nullptr) == 0) {
                    MessageBoxW(
                        window, L"Таймер захвата недоступен.", L"Добавить кадр", MB_OK | MB_ICONERROR);
                    return 0;
                }
                ShowWindow(window, SW_HIDE);
                return 0;
            case kRemoveFrame:
                removeFrame(*state);
                return 0;
            case kSavePng:
                save(*state, false);
                return 0;
            case kSavePackage:
                save(*state, true);
                return 0;
            case kCancel:
                DestroyWindow(window);
                return 0;
            }
            break;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_NCDESTROY:
            KillTimer(window, kCaptureTimer);
            state->window = nullptr;
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            break;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

} // namespace

bool showReportCardPreview(HWND owner, const GameDiagnosisInput& input, HWND gameWindow, std::string& error)
{
    PreviewState state;
    state.input = input;
    state.gameWindow = gameWindow;
    if (!renderReportCard(input, nullptr, { }, state.card, error))
        return false;
    WNDCLASSW cls { };
    cls.lpfnWndProc = previewProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kPreviewClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (RegisterClassW(&cls) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        error = "Предпросмотр: не удалось зарегистрировать окно";
        return false;
    }
    HWND preview = CreateWindowExW(WS_EX_DLGMODALFRAME, kPreviewClass,
        L"Проверьте содержимое перед сохранением · YawaFpsHub", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
        CW_USEDEFAULT, 1080, 740, owner, nullptr, cls.hInstance, &state);
    if (!preview) {
        error = "Предпросмотр: не удалось открыть окно";
        return false;
    }
    EnableWindow(owner, FALSE);
    ShowWindow(preview, SW_SHOW);
    UpdateWindow(preview);
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
        error = "Предпросмотр: ошибка обработки сообщений окна";
        return false;
    }
    return true;
}

} // namespace yawa
