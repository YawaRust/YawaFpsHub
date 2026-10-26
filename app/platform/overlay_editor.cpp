#include "platform/overlay_editor.h"
#include "metrics/overlay_rows.h"
#include "platform/win_util.h"

#include <windowsx.h>

#include <algorithm>
#include <utility>

namespace yawa {
namespace {
    constexpr wchar_t kEditorClass[] = L"YawaFpsHubOverlayEditor";
    constexpr int kCanvasLeft = 18;
    constexpr int kCanvasTop = 82;
    constexpr int kCanvasWidth = 520;
    constexpr int kViewportHeight = 490;
    constexpr int kMaxY = 1490;

    void text(HDC dc, std::wstring_view value, RECT rect, COLORREF color)
    {
        SetTextColor(dc, color);
        DrawTextW(dc, value.data(), static_cast<int>(value.size()), &rect,
            DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
} // namespace

OverlayEditor::~OverlayEditor()
{
    close();
}

void OverlayEditor::close()
{
    if (m_window) {
        DestroyWindow(m_window);
    }
}

void OverlayEditor::refresh()
{
    if (m_window) {
        InvalidateRect(m_window, nullptr, FALSE);
    }
}

bool OverlayEditor::open(HWND owner, HINSTANCE instance, OverlaySettings& settings, const MetricStore& store,
    std::function<void(bool)> onChange)
{
    if (m_window) {
        ShowWindow(m_window, SW_RESTORE);
        SetForegroundWindow(m_window);
        return true;
    }
    WNDCLASSW cls { };
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kEditorClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
    cls.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }
    m_settings = &settings;
    m_store = &store;
    m_onChange = std::move(onChange);
    enableOverlayPixelLayout(settings);
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kEditorClass, L"Редактор оверлея · YawaFpsHub",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 750, 690, owner, nullptr, instance, this);
    if (!m_window) {
        m_settings = nullptr;
        m_store = nullptr;
        return false;
    }
    ShowWindow(m_window, SW_SHOW);
    m_onChange(true);
    return true;
}

LRESULT CALLBACK OverlayEditor::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        auto* editor = static_cast<OverlayEditor*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
        return TRUE;
    }
    auto* editor = reinterpret_cast<OverlayEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return editor ? editor->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

void OverlayEditor::move(int x, int y)
{
    if (!m_dragging || !m_settings) {
        return;
    }
    auto& row = m_settings->rowPositions[static_cast<std::size_t>(m_selected)];
    m_settings->pixelLayout = true;
    int& targetX = m_value ? row.valueX : row.labelX;
    targetX = std::clamp(x - kCanvasLeft - m_grabX, 0, 470);
    row.y = std::clamp(y - kCanvasTop + m_scroll - m_grabY, 0, kMaxY);
    m_onChange(false);
    InvalidateRect(m_window, nullptr, FALSE);
}

LRESULT OverlayEditor::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        DestroyWindow(m_window);
        return 0;
    case WM_NCDESTROY: {
        if (m_dragging) {
            ReleaseCapture();
        }
        m_dragging = false;
        const HWND window = m_window;
        m_window = nullptr;
        m_settings = nullptr;
        m_store = nullptr;
        m_onChange = { };
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        paint();
        return 0;
    case WM_MOUSEWHEEL:
        m_scroll = std::clamp(m_scroll - GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 60, 0, 1050);
        InvalidateRect(m_window, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (y >= 45 && y <= 70 && x >= 550 && x < 720) {
            m_settings->pixelLayout = false;
            m_onChange(true);
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        if (x < kCanvasLeft || x >= kCanvasLeft + kCanvasWidth || y < kCanvasTop
            || y >= kCanvasTop + kViewportHeight) {
            return 0;
        }
        const auto rows = overlayMetricRows(*m_settings, *m_store);
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
            auto& row = m_settings->rowPositions[static_cast<std::size_t>(it->id)];
            const int rowY = row.y - m_scroll + kCanvasTop;
            if (y < rowY || y > rowY + 25) {
                continue;
            }
            if (x >= kCanvasLeft + row.valueX && x < kCanvasLeft + 510) {
                m_selected = it->id;
                m_value = true;
                m_grabX = x - kCanvasLeft - row.valueX;
            } else if (x >= kCanvasLeft + row.labelX && x < kCanvasLeft + row.labelX + 230) {
                m_selected = it->id;
                m_value = false;
                m_grabX = x - kCanvasLeft - row.labelX;
            } else {
                continue;
            }
            m_grabY = y - rowY;
            m_dragging = true;
            SetFocus(m_window);
            SetCapture(m_window);
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_dragging) {
            move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_dragging) {
            move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            m_dragging = false;
            ReleaseCapture();
            m_onChange(true);
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            close();
            return 0;
        }
        if (wParam == VK_TAB) {
            m_value = !m_value;
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
            m_settings->pixelLayout = true;
            auto& row = m_settings->rowPositions[static_cast<std::size_t>(m_selected)];
            const int delta = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
            if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                int& x = m_value ? row.valueX : row.labelX;
                x = std::clamp(x + (wParam == VK_LEFT ? -delta : delta), 0, 470);
            } else {
                row.y = std::clamp(row.y + (wParam == VK_UP ? -delta : delta), 0, kMaxY);
            }
            m_onChange(true);
            InvalidateRect(m_window, nullptr, FALSE);
            return 0;
        }
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void OverlayEditor::paint()
{
    PAINTSTRUCT ps { };
    HDC dc = BeginPaint(m_window, &ps);
    RECT client { };
    GetClientRect(m_window, &client);
    HBRUSH background = CreateSolidBrush(RGB(14, 19, 27));
    FillRect(dc, &client, background);
    DeleteObject(background);
    SetBkMode(dc, TRANSPARENT);
    text(dc, L"Двигайте подпись и значение независимо · стрелки: 1 px · Shift: 10 px · Tab: выбор",
        RECT { 18, 8, 730, 40 }, RGB(224, 238, 242));
    text(dc, L"Колесо: прокрутка холста · Esc: закрыть", RECT { 18, 40, 540, 70 }, RGB(146, 170, 184));
    text(dc, L"Вернуть шаблон", RECT { 550, 45, 720, 70 }, RGB(112, 231, 191));
    RECT viewport { kCanvasLeft, kCanvasTop, kCanvasLeft + kCanvasWidth, kCanvasTop + kViewportHeight };
    HBRUSH canvas = CreateSolidBrush(RGB(25, 35, 46));
    FillRect(dc, &viewport, canvas);
    DeleteObject(canvas);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, viewport.left, viewport.top, viewport.right, viewport.bottom);
    HPEN grid = CreatePen(PS_SOLID, 1, RGB(38, 50, 61));
    HGDIOBJ old = SelectObject(dc, grid);
    for (int y = -m_scroll % 20; y < kViewportHeight; y += 20) {
        MoveToEx(dc, kCanvasLeft, kCanvasTop + y, nullptr);
        LineTo(dc, kCanvasLeft + kCanvasWidth, kCanvasTop + y);
    }
    SelectObject(dc, old);
    DeleteObject(grid);
    for (const auto& item : overlayMetricRows(*m_settings, *m_store)) {
        const auto& row = m_settings->rowPositions[static_cast<std::size_t>(item.id)];
        if (row.y < m_scroll - 25 || row.y > m_scroll + kViewportHeight) {
            continue;
        }
        const int y = kCanvasTop + row.y - m_scroll;
        const bool active = m_selected == item.id;
        text(dc, widen(item.label),
            RECT {
                kCanvasLeft + row.labelX, y, kCanvasLeft + std::min(row.labelX + 230, kCanvasWidth), y + 26 },
            active && !m_value ? RGB(109, 243, 197) : RGB(192, 214, 221));
        text(dc, widen(item.value),
            RECT { kCanvasLeft + row.valueX, y, kCanvasLeft + kCanvasWidth - 8, y + 26 },
            active && m_value ? RGB(109, 243, 197) : RGB(251, 252, 255));
    }
    RestoreDC(dc, saved);
    text(dc, L"Холст 520 px · реальные шрифты/цвета смотрите в предпросмотре приложения",
        RECT { 18, 582, 720, 610 }, RGB(146, 170, 184));
    EndPaint(m_window, &ps);
}

} // namespace yawa
