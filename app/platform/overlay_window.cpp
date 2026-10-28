#include "platform/overlay_window.h"
#include "platform/overlay_monitor.h"
#include "platform/win_util.h"
#include "ui/canvas_transform.h"
#include "ui/layout.h"

#include <d2d1helper.h>
#include <dwrite.h>
#include <windowsx.h>

#include <algorithm>
#include <utility>
#include <wincodec.h>

#include <cstring>
#include <cmath>

namespace yawa {
namespace {

constexpr wchar_t kClassName[] = L"YawaFpsHubOverlay";
IDWriteTextFormat* createFormat(IDWriteFactory* factory, float size, DWRITE_FONT_WEIGHT weight,
    DWRITE_TEXT_ALIGNMENT alignment, const wchar_t* family)
{
    IDWriteTextFormat* format = nullptr;
    if (FAILED(factory->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
            size, L"ru-RU", &format))
        || format == nullptr) {
        return nullptr;
    }
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetTextAlignment(alignment);
    return format;
}

} // namespace

OverlayWindow::~OverlayWindow()
{
    destroy();
}

LRESULT CALLBACK OverlayWindow::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    OverlayWindow* self = reinterpret_cast<OverlayWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
    case WM_NCCREATE: {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return DefWindowProcW(window, message, wParam, lParam);
    }

    case WM_NCHITTEST:
        // Окно прозрачно для мыши везде, кроме самого оверлея: клики по игре проходят,
        // а оверлей можно взять обычным нажатием левой кнопки и перетащить куда угодно.
        if (self != nullptr && self->m_settings.clickThrough) {
            // Сквозной клик: окно не забирает мышь даже над оверлеем, всё уходит в игру.
            return HTTRANSPARENT;
        }
        if (self != nullptr) {
            POINT point { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
            ScreenToClient(window, &point);
            const D2D1_RECT_F& area = self->m_contentRectD2D;
            const float fx = static_cast<float>(point.x);
            const float fy = static_cast<float>(point.y);
            if (area.right > area.left && fx >= area.left - 4.0f && fx <= area.right + 4.0f
                && fy >= area.top - 4.0f && fy <= area.bottom + 4.0f) {
                return HTCLIENT;
            }
        }
        return HTTRANSPARENT;

    case WM_SETCURSOR:
        if (self != nullptr && !self->m_settings.clickThrough) {
            POINT cursor { };
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            const D2D1_RECT_F& area = self->m_contentRectD2D;
            const bool overContent = area.right > area.left
                && static_cast<float>(cursor.x) >= area.left - 4.0f
                && static_cast<float>(cursor.x) <= area.right + 4.0f
                && static_cast<float>(cursor.y) >= area.top - 4.0f
                && static_cast<float>(cursor.y) <= area.bottom + 4.0f;
            if (overContent) {
                const wchar_t* shape = self->onResizeCorner(cursor.x, cursor.y) ? IDC_SIZENWSE
                    : self->layerAt(cursor.x, cursor.y) >= 0 || self->rowAt(cursor.x, cursor.y).has_value()
                    ? IDC_HAND
                    : IDC_SIZEALL;
                SetCursor(LoadCursorW(nullptr, shape));
                return TRUE;
            }
        }
        break;

    case WM_LBUTTONDOWN:
        if (self != nullptr && !self->m_settings.clickThrough) {
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (self->onResizeCorner(x, y)) {
                self->m_resizing = true;
                self->m_resizeOrigin = POINT { x, y };
                self->m_resizeScale = self->m_settings.scalePercent;
                self->m_resizeWidth = self->m_contentRectD2D.right - self->m_contentRectD2D.left;
                self->m_resizeHeight = self->m_contentRectD2D.bottom - self->m_contentRectD2D.top;
            } else if (self->m_onLayerEdit && GetKeyState(VK_MENU) >= 0
                && self->layerAt(x, y) >= 0) {
                self->m_layerIndex = static_cast<std::size_t>(self->layerAt(x, y));
                self->m_layerDragging = true;
                self->m_layerDragOrigin = POINT { x, y };
                self->m_layerDragScale = self->m_rtssLayerScale;
                const RtssLayerEdit& edit = self->m_settings.rtssLayerEdits[self->m_layerIndex];
                self->m_layerStartX = edit.x;
                self->m_layerStartY = edit.y;
            } else if (self->m_onRowEdit && GetKeyState(VK_MENU) >= 0 && self->rowAt(x, y)) {
                self->m_rowDragId = *self->rowAt(x, y);
                self->m_rowDragging = true;
                self->m_rowDragOrigin = POINT { x, y };
                self->m_rowStart = self->m_settings.rowPositions[static_cast<std::size_t>(self->m_rowDragId)];
                self->m_rowDragScale
                    = std::max(0.001f, (self->m_contentRectD2D.right - self->m_contentRectD2D.left) / 520.0f);
                if (self->m_rowStart.y < 0) {
                    const auto entry = std::find_if(self->m_rowBounds.begin(), self->m_rowBounds.end(),
                        [self](const OverlayRowBounds& row) { return row.id == self->m_rowDragId; });
                    self->m_rowStart.y = static_cast<int>(
                        std::lround((entry->rect.top - self->m_contentRectD2D.top) / self->m_rowDragScale));
                }
            } else {
                self->beginDrag(x + static_cast<int>(self->m_monitorRect.left),
                    y + static_cast<int>(self->m_monitorRect.top));
            }
            SetCapture(window);
            return 0;
        }
        break;

    case WM_RBUTTONUP:
        // Вернуться в приложение можно прямо с оверлея, не разыскивая иконку в трее.
        if (self != nullptr && !self->m_settings.clickThrough && self->m_owner != nullptr) {
            ShowWindow(self->m_owner, SW_RESTORE);
            SetForegroundWindow(self->m_owner);
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (self != nullptr && self->m_resizing) {
            self->updateResize(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        if (self != nullptr && self->m_layerDragging) {
            self->updateLayerDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        if (self != nullptr && self->m_rowDragging) {
            self->updateRowDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            return 0;
        }
        if (self != nullptr && self->m_dragging) {
            self->updateDrag(GET_X_LPARAM(lParam) + static_cast<int>(self->m_monitorRect.left),
                GET_Y_LPARAM(lParam) + static_cast<int>(self->m_monitorRect.top));
            return 0;
        }
        break;

    case WM_MOUSEWHEEL:
        if (self != nullptr && !self->m_settings.clickThrough && self->m_onDirectEdit) {
            const bool ctrl = (GET_KEYSTATE_WPARAM(wParam) & MK_CONTROL) != 0;
            const bool shift = (GET_KEYSTATE_WPARAM(wParam) & MK_SHIFT) != 0;
            if (!ctrl && !shift) {
                break;
            }
            const int step = GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? 1 : -1;
            if (ctrl && shift) {
                self->m_settings.rowGapPx = std::clamp(
                    (self->m_settings.rowGapPx < 0 ? 3 : self->m_settings.rowGapPx) + step * 2, 0, 14);
            } else if (ctrl) {
                self->m_settings.scalePercent = std::clamp(self->m_settings.scalePercent + step * 5, 70, 160);
            } else {
                self->m_settings.paddingPx = std::clamp(
                    (self->m_settings.paddingPx < 0 ? 10 : self->m_settings.paddingPx) + step * 2, 0, 28);
            }
            self->repaint();
            self->m_onDirectEdit(
                self->m_settings.scalePercent, self->m_settings.paddingPx, self->m_settings.rowGapPx, true);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (self != nullptr && self->m_resizing) {
            self->finishResize();
            ReleaseCapture();
            return 0;
        }
        if (self != nullptr && self->m_layerDragging) {
            self->updateLayerDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            self->finishLayerDrag();
            ReleaseCapture();
            return 0;
        }
        if (self != nullptr && self->m_rowDragging) {
            self->updateRowDrag(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
            self->finishRowDrag();
            ReleaseCapture();
            return 0;
        }
        if (self != nullptr && self->m_dragging) {
            self->endDrag();
            ReleaseCapture();
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:
        if (self != nullptr) {
            self->finishResize();
            self->finishLayerDrag();
            self->finishRowDrag();
            self->endDrag();
        }
        break;

    case WM_TIMER:
        if (self != nullptr && wParam == 1) {
            self->tickAnimation();
            return 0;
        }
        break;

    case WM_ERASEBKGND:
        return 1;

    default:
        break;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}

bool OverlayWindow::create(HINSTANCE instance, std::string& error)
{
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &OverlayWindow::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kClassName;
    RegisterClassExW(&windowClass);

    m_monitorRect = screenRect();
    const int screenWidth = m_monitorRect.right - m_monitorRect.left;
    const int screenHeight = m_monitorRect.bottom - m_monitorRect.top;

    const DWORD extendedStyle = WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE
        | (m_settings.clickThrough ? WS_EX_TRANSPARENT : 0);
    m_window = CreateWindowExW(extendedStyle, kClassName, L"YawaFpsHub overlay", WS_POPUP, m_monitorRect.left,
        m_monitorRect.top, screenWidth, screenHeight, nullptr, nullptr, instance, this);

    if (m_window == nullptr) {
        error = "не удалось создать окно оверлея";
        return false;
    }

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(&m_dwrite)))) {
        error = "DirectWrite не инициализировался для оверлея";
        return false;
    }

    m_keyFormat = createFormat(m_dwrite, 10.0f, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,
        L"Segoe UI");
    m_valueFormat = createFormat(m_dwrite, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_TRAILING,
        L"Segoe UI");
    m_smallFormat = createFormat(m_dwrite, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING,
        L"Segoe UI");
    m_monoFormat = createFormat(m_dwrite, 11.0f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING,
        L"Cascadia Mono");

    // До первого опроса неизвестные метрики показываются как «нет данных»,
    // а не маскируются нулями или пустым оверлеем.
    m_hasContent = true;
    return true;
}

RECT OverlayWindow::screenRect() const
{
    // Виртуальный экран: оверлей занимает все мониторы сразу, поэтому его можно
    // перетащить на соседний экран, а не только в пределах одного.
    RECT rect{};
    rect.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    rect.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    rect.right = rect.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    rect.bottom = rect.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        rect = RECT{ 0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN) };
    }
    return rect;
}

void OverlayWindow::destroy()
{
    if (m_target != nullptr) {
        m_target->Release();
        m_target = nullptr;
    }
    if (m_brush != nullptr) {
        m_brush->Release();
        m_brush = nullptr;
    }
    IDWriteTextFormat** formats[] = { &m_keyFormat, &m_valueFormat, &m_smallFormat, &m_monoFormat };
    for (IDWriteTextFormat** format : formats) {
        if (*format != nullptr) {
            (*format)->Release();
            *format = nullptr;
        }
    }
    if (m_dwrite != nullptr) {
        m_dwrite->Release();
        m_dwrite = nullptr;
    }
    if (m_bitmap != nullptr) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
        m_pixels = nullptr;
    }
    if (m_memoryDc != nullptr) {
        DeleteDC(m_memoryDc);
        m_memoryDc = nullptr;
    }
    if (m_window != nullptr) {
        DestroyWindow(m_window);
        m_window = nullptr;
    }
}

void OverlayWindow::setAccent(Accent accent)
{
    m_theme = makeTheme(accent);
    repaint();
}

void OverlayWindow::setGameMonitor(const RECT* monitor)
{
    if (monitor == nullptr || monitor->right <= monitor->left || monitor->bottom <= monitor->top) {
        m_gameMonitor.reset();
    } else {
        m_gameMonitor = *monitor;
    }
}

void OverlayWindow::setMenuSceneOpacity(std::optional<int> opacity)
{
    m_menuSceneOpacity = opacity;
    repaint();
}

void OverlayWindow::setSettings(const OverlaySettings& settings)
{
    // Плавный переход считаем от текущего видимого состояния, а не от нового:
    // иначе анимировать было бы нечего. Сдвиг и подписи не анимируем — при
    // перетаскивании setSettings приходит на каждое движение мыши.
    const OverlaySettings previous = animatedSettings();
    // Анимируем только «крупные» изменения: стиль, геометрию и прозрачность. Цвета,
    // отступы, шрифт и подписи применяются сразу — иначе каждый пиксель ползунка
    // перезапускал анимацию, и оверлей дрожал вместо плавного изменения.
    const bool styleChanged = settings.style != m_settings.style;
    const bool chromeChanged = settings.layout != m_settings.layout
        || settings.position != m_settings.position || settings.showBackground != m_settings.showBackground
        || settings.showBorder != m_settings.showBorder || settings.showShadow != m_settings.showShadow
        || settings.cornerRadius != m_settings.cornerRadius
        || settings.scalePercent != m_settings.scalePercent
        || settings.opacityPercent != m_settings.opacityPercent
        || settings.backgroundOpacityPercent != m_settings.backgroundOpacityPercent;
    const bool movedOnly = settings.offsetX != m_settings.offsetX || settings.offsetY != m_settings.offsetY
        || settings.originX != m_settings.originX || settings.originY != m_settings.originY
        || settings.anchorMode != m_settings.anchorMode
        || settings.followGameMonitor != m_settings.followGameMonitor;
    m_settings = settings;
    if (m_window != nullptr) {
        // HTTRANSPARENT передаёт hit-test только окнам того же потока. Для игры
        // в другом процессе layered-окну нужен WS_EX_TRANSPARENT.
        const LONG_PTR extendedStyle = GetWindowLongPtrW(m_window, GWL_EXSTYLE);
        const LONG_PTR nextStyle = settings.clickThrough
            ? extendedStyle | WS_EX_TRANSPARENT
            : extendedStyle & ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
        if (nextStyle != extendedStyle) {
            SetWindowLongPtrW(m_window, GWL_EXSTYLE, nextStyle);
            SetWindowPos(m_window, nullptr, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        if (settings.clickThrough) {
            if (GetCapture() == m_window) {
                ReleaseCapture();
            }
            endDrag();
            finishResize();
            finishLayerDrag();
            finishRowDrag();
        }
    }

    // Сдвиг и «мелкие» настройки рисуем сразу, без анимации.
    if (movedOnly || (!styleChanged && !chromeChanged)) {
        repaint();
        return;
    }
    // Пакет RTSS может не иметь ни одной знакомой метрики. Не показываем
    // прозрачный первый кадр при смене шаблона, иначе он выглядит исчезнувшим.
    const bool importedLayout
        = settings.rtssVisual && settings.rtssTemplate && settings.rtssTemplate != previous.rtssTemplate;
    startAnimation(previous, styleChanged && !importedLayout);
    if (importedLayout) {
        m_fade = 1.0f;
    }
    repaint();
}

void OverlayWindow::setMoveHandler(std::function<void(int, int, bool)> handler)
{
    m_onMove = std::move(handler);
}

void OverlayWindow::setDirectEditHandler(std::function<void(int, int, int, bool)> handler)
{
    m_onDirectEdit = std::move(handler);
}

bool OverlayWindow::onResizeCorner(int x, int y) const
{
    const float width = m_contentRectD2D.right - m_contentRectD2D.left;
    const float height = m_contentRectD2D.bottom - m_contentRectD2D.top;
    return width >= 60.0f && height >= 45.0f && x >= m_contentRectD2D.right - 16.0f
        && x <= m_contentRectD2D.right + 4.0f && y >= m_contentRectD2D.bottom - 16.0f
        && y <= m_contentRectD2D.bottom + 4.0f;
}

void OverlayWindow::updateResize(int x, int y)
{
    if (!m_resizing) {
        return;
    }
    const float deltaX = static_cast<float>(x - m_resizeOrigin.x) / std::max(1.0f, m_resizeWidth);
    const float deltaY = static_cast<float>(y - m_resizeOrigin.y) / std::max(1.0f, m_resizeHeight);
    const int next = std::clamp(
        static_cast<int>(std::lround(m_resizeScale * (1.0f + (deltaX + deltaY) / 2.0f))), 70, 160);
    if (next != m_settings.scalePercent) {
        m_settings.scalePercent = next;
        repaint();
        if (m_onDirectEdit) {
            m_onDirectEdit(next, m_settings.paddingPx, m_settings.rowGapPx, false);
        }
    }
}

void OverlayWindow::finishResize()
{
    if (!m_resizing) {
        return;
    }
    m_resizing = false;
    if (m_onDirectEdit) {
        m_onDirectEdit(m_settings.scalePercent, m_settings.paddingPx, m_settings.rowGapPx, true);
    }
}

void OverlayWindow::setLayerEditHandler(std::function<void(std::size_t, int, int, bool)> handler)
{
    m_onLayerEdit = std::move(handler);
}

int OverlayWindow::layerAt(int x, int y) const
{
    if (!m_settings.rtssTemplate || !m_settings.rtssVisual || m_settings.recipe != OverlayRecipe::Rtss) {
        return -1;
    }
    for (std::size_t index = m_rtssLayerBounds.size(); index > 0; --index) {
        const Rect& box = m_rtssLayerBounds[index - 1];
        if (box.right > box.left && box.contains(static_cast<float>(x), static_cast<float>(y))) {
            return static_cast<int>(index - 1);
        }
    }
    return -1;
}

void OverlayWindow::updateLayerDrag(int x, int y)
{
    if (!m_layerDragging || !m_onLayerEdit || m_layerIndex >= m_settings.rtssLayerEdits.size()) {
        return;
    }
    const float unit = std::max(0.001f, m_layerDragScale);
    const int nextX = std::clamp(m_layerStartX
            + static_cast<int>(std::lround((x - m_layerDragOrigin.x) / unit)), -2048, 2048);
    const int nextY = std::clamp(m_layerStartY
            + static_cast<int>(std::lround((y - m_layerDragOrigin.y) / unit)), -2048, 2048);
    RtssLayerEdit& edit = m_settings.rtssLayerEdits[m_layerIndex];
    if (edit.x != nextX || edit.y != nextY) {
        edit.x = nextX;
        edit.y = nextY;
        repaint();
        m_onLayerEdit(m_layerIndex, nextX, nextY, false);
    }
}

void OverlayWindow::finishLayerDrag()
{
    if (!m_layerDragging) {
        return;
    }
    m_layerDragging = false;
    if (m_onLayerEdit && m_layerIndex < m_settings.rtssLayerEdits.size()) {
        const RtssLayerEdit& edit = m_settings.rtssLayerEdits[m_layerIndex];
        m_onLayerEdit(m_layerIndex, edit.x, edit.y, true);
    }
}

void OverlayWindow::setRowEditHandler(std::function<void(MetricId, OverlayRowPosition, bool)> handler)
{
    m_onRowEdit = std::move(handler);
}

std::optional<MetricId> OverlayWindow::rowAt(int x, int y) const
{
    if (!m_settings.pixelLayout || m_settings.layout != OverlayLayout::Plates) {
        return std::nullopt;
    }
    for (auto it = m_rowBounds.rbegin(); it != m_rowBounds.rend(); ++it) {
        if (it->rect.contains(static_cast<float>(x), static_cast<float>(y))) {
            return it->id;
        }
    }
    return std::nullopt;
}

void OverlayWindow::updateRowDrag(int x, int y)
{
    if (!m_rowDragging || !m_onRowEdit) {
        return;
    }
    const int dx = static_cast<int>(std::lround((x - m_rowDragOrigin.x) / m_rowDragScale));
    const int dy = static_cast<int>(std::lround((y - m_rowDragOrigin.y) / m_rowDragScale));
    const OverlayRowPosition next = moveOverlayRow(m_rowStart, dx, dy);
    auto& current = m_settings.rowPositions[static_cast<std::size_t>(m_rowDragId)];
    if (current.labelX != next.labelX || current.valueX != next.valueX || current.y != next.y) {
        current = next;
        repaint();
        m_onRowEdit(m_rowDragId, next, false);
    }
}

void OverlayWindow::finishRowDrag()
{
    if (!m_rowDragging) {
        return;
    }
    m_rowDragging = false;
    if (m_onRowEdit) {
        m_onRowEdit(m_rowDragId, m_settings.rowPositions[static_cast<std::size_t>(m_rowDragId)], true);
    }
}

void OverlayWindow::beginDrag(int screenX, int screenY)
{
    m_dragging = true;
    m_dragOrigin = POINT { screenX, screenY };
    m_dragOffsetX
        = m_settings.anchorMode == OverlayAnchorMode::Free ? m_settings.originX : m_settings.offsetX;
    m_dragOffsetY
        = m_settings.anchorMode == OverlayAnchorMode::Free ? m_settings.originY : m_settings.offsetY;
}

void OverlayWindow::updateDrag(int screenX, int screenY)
{
    if (!m_dragging) {
        return;
    }
    const int deltaX = screenX - static_cast<int>(m_dragOrigin.x);
    const int deltaY = screenY - static_cast<int>(m_dragOrigin.y);
    int x = 0;
    int y = 0;
    if (m_settings.anchorMode == OverlayAnchorMode::Free) {
        x = m_settings.originX = std::clamp(m_dragOffsetX + deltaX, 0,
            std::max(0,
                static_cast<int>(m_bitmapWidth)
                    - static_cast<int>(m_contentRectD2D.right - m_contentRectD2D.left)));
        y = m_settings.originY = std::clamp(m_dragOffsetY + deltaY, 0,
            std::max(0,
                static_cast<int>(m_bitmapHeight)
                    - static_cast<int>(m_contentRectD2D.bottom - m_contentRectD2D.top)));
    } else {
        x = m_settings.offsetX = std::clamp(m_dragOffsetX + deltaX, -8192, 8192);
        y = m_settings.offsetY = std::clamp(m_dragOffsetY + deltaY, -8192, 8192);
    }
    repaint();
    if (m_onMove) {
        m_onMove(x, y, false);
    }
}

void OverlayWindow::endDrag()
{
    if (!m_dragging) {
        return;
    }
    m_dragging = false;
    if (m_onMove) {
        m_onMove(m_settings.anchorMode == OverlayAnchorMode::Free ? m_settings.originX : m_settings.offsetX,
            m_settings.anchorMode == OverlayAnchorMode::Free ? m_settings.originY : m_settings.offsetY, true);
    }
}

void OverlayWindow::startAnimation(const OverlaySettings& from, bool fadeContent)
{
    if (!m_visible) {
        m_animProgress = 1.0f;
        m_animating = false;
        return;
    }

    // Плавный переход к новым значениям: переключение стилей больше не «дёргается».
    m_animFromScale = static_cast<float>(from.scalePercent) / 100.0f;
    m_animFromOpacity = static_cast<float>(from.opacityPercent) / 100.0f;
    m_animFromRadius = static_cast<float>(from.cornerRadius);
    m_animToScale = static_cast<float>(m_settings.scalePercent) / 100.0f;
    m_animToOpacity = static_cast<float>(m_settings.opacityPercent) / 100.0f;
    m_animToRadius = static_cast<float>(m_settings.cornerRadius);
    // Если анимация уже идёт, продолжаем с середины: значения догоняют ползунок
    // без рывка и без паузы в четверть секунды.
    m_animProgress = m_animating ? 0.35f : 0.0f;
    if (fadeContent) {
        m_fade = 0.0f;
    }
    m_animating = true;
    if (m_window != nullptr) {
        SetTimer(m_window, 1, 16, nullptr);
    }
}

void OverlayWindow::tickAnimation()
{
    if (!m_animating || !m_visible) {
        m_animating = false;
        if (m_window != nullptr) {
            KillTimer(m_window, 1);
        }
        return;
    }

    m_animProgress = std::min(1.0f, m_animProgress + 0.14f);
    // Появление считаем по той же кривой: содержимое не «щёлкает» при смене стиля.
    m_fade = 1.0f - (1.0f - m_animProgress) * (1.0f - m_animProgress);
    if (m_animProgress >= 1.0f) {
        m_animating = false;
        if (m_window != nullptr) {
            KillTimer(m_window, 1);
        }
    }
    repaint();
}

OverlaySettings OverlayWindow::animatedSettings() const
{
    if (!m_animating) {
        return m_settings;
    }

    // ease-out: быстрый старт и мягкое завершение, как у переходов в демо.
    const float eased = 1.0f - (1.0f - m_animProgress) * (1.0f - m_animProgress);
    OverlaySettings result = m_settings;
    result.scalePercent = static_cast<int>(std::lround((m_animFromScale + (m_animToScale - m_animFromScale) * eased)
        * 100.0f));
    result.opacityPercent = static_cast<int>(std::lround(
        (m_animFromOpacity + (m_animToOpacity - m_animFromOpacity) * eased) * 100.0f));
    if (m_animFromRadius >= 0.0f && m_animToRadius >= 0.0f) {
        result.cornerRadius = static_cast<int>(
            std::lround(m_animFromRadius + (m_animToRadius - m_animFromRadius) * eased));
    }
    return result;
}

void OverlayWindow::setContent(const MetricStore& store, const FpsSummary& fps, const std::wstring& gpuName)
{
    m_store = store;
    m_fps = fps;
    m_gpuName = gpuName;
    m_hasContent = true;
}

bool OverlayWindow::show(std::string* error)
{
    if (m_window == nullptr) {
        if (error != nullptr) {
            *error = "окно оверлея не создано";
        }
        return false;
    }

    // Монитор пересчитываем на каждом показе: пользователь мог перетащить игру на другой.
    m_monitorRect = screenRect();
    const int width = m_monitorRect.right - m_monitorRect.left;
    const int height = m_monitorRect.bottom - m_monitorRect.top;

    SetWindowPos(m_window, HWND_TOPMOST, m_monitorRect.left, m_monitorRect.top, width, height,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    m_visible = true;

    if (!render()) {
        m_visible = false;
        ShowWindow(m_window, SW_HIDE);
        if (error != nullptr) {
            *error = "не удалось нарисовать оверлей";
        }
        return false;
    }

    ShowWindow(m_window, SW_SHOWNOACTIVATE);
    return true;
}

void OverlayWindow::hide()
{
    m_gameMonitor.reset();
    if (m_window == nullptr) {
        return;
    }
    m_visible = false;
    m_animating = false;
    m_fade = 1.0f;
    KillTimer(m_window, 1);
    ShowWindow(m_window, SW_HIDE);
}

void OverlayWindow::toggle(std::string* error)
{
    if (m_visible) {
        hide();
    } else {
        show(error);
    }
}

bool OverlayWindow::ensureRenderTarget(UINT width, UINT height)
{
    if (m_window == nullptr) {
        return false;
    }

    if (m_target == nullptr) {
        ID2D1Factory* factory = nullptr;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &factory))) {
            return false;
        }

        // 8-битная SDR-поверхность: Windows DWM выполняет SDRBoost сам для HDR-монитора.
        // FP16 без ручной поправки уровня белого сделал бы те же цвета темнее.
        const D2D1_RENDER_TARGET_PROPERTIES properties
            = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

        HRESULT result = factory->CreateDCRenderTarget(&properties, &m_target);
        if (FAILED(result) || m_target == nullptr) {
            // Запасной путь: программный рендер. Именно он нужен в RDP и на машинах
            // без аппаратного ускорения Direct2D.
            const D2D1_RENDER_TARGET_PROPERTIES software = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_SOFTWARE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            result = factory->CreateDCRenderTarget(&software, &m_target);
        }
        factory->Release();
        if (FAILED(result) || m_target == nullptr) {
            return false;
        }
    }

    if (m_bitmapWidth == width && m_bitmapHeight == height && m_memoryDc != nullptr) {
        return true;
    }

    if (m_bitmap != nullptr) {
        DeleteObject(m_bitmap);
        m_bitmap = nullptr;
        m_pixels = nullptr;
    }
    if (m_memoryDc == nullptr) {
        m_memoryDc = CreateCompatibleDC(nullptr);
    }

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    m_bitmap = CreateDIBSection(m_memoryDc, &info, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
    if (m_bitmap == nullptr) {
        return false;
    }

    SelectObject(m_memoryDc, m_bitmap);
    m_bitmapWidth = width;
    m_bitmapHeight = height;

    const RECT bounds{ 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    m_target->BindDC(m_memoryDc, &bounds);
    return true;
}

void OverlayWindow::repaint()
{
    if (!m_visible) {
        return;
    }
    render();
}

bool OverlayWindow::hasVisiblePixels() const
{
    if (m_pixels == nullptr || m_contentRectD2D.right <= m_contentRectD2D.left
        || m_contentRectD2D.bottom <= m_contentRectD2D.top) {
        return false;
    }
    const auto left
        = static_cast<UINT>(std::clamp(m_contentRectD2D.left, 0.0f, static_cast<float>(m_bitmapWidth)));
    const auto top
        = static_cast<UINT>(std::clamp(m_contentRectD2D.top, 0.0f, static_cast<float>(m_bitmapHeight)));
    const auto right
        = static_cast<UINT>(std::clamp(m_contentRectD2D.right, 0.0f, static_cast<float>(m_bitmapWidth)));
    const auto bottom
        = static_cast<UINT>(std::clamp(m_contentRectD2D.bottom, 0.0f, static_cast<float>(m_bitmapHeight)));
    const auto* pixels = static_cast<const std::uint8_t*>(m_pixels);
    for (UINT y = top; y < bottom; ++y) {
        for (UINT x = left; x < right; ++x) {
            if (pixels[(static_cast<std::size_t>(y) * m_bitmapWidth + x) * 4 + 3] != 0) {
                return true;
            }
        }
    }
    return false;
}

void OverlayWindow::drawScene(float scale)
{
    OverlayFormats formats{ m_dwrite, m_keyFormat, m_valueFormat, m_smallFormat, m_monoFormat };
    const OverlayText text = makeOverlayText(m_settings, m_store);
    Rect bounds { 0.0f, 0.0f, static_cast<float>(m_bitmapWidth), static_cast<float>(m_bitmapHeight) };
    OverlaySettings painted = m_settings;
    RECT savedMonitor { };
    const bool savedAvailable = painted.anchorMode == OverlayAnchorMode::Free
        && findOverlayMonitor(painted.savedMonitorId, savedMonitor);
    if (savedAvailable) {
        painted.originX = originFromMonitor(painted.monitorOriginX, savedMonitor.left, m_monitorRect.left);
        painted.originY = originFromMonitor(painted.monitorOriginY, savedMonitor.top, m_monitorRect.top);
    }
    if (m_settings.followGameMonitor && m_gameMonitor) {
        const RECT& game = *m_gameMonitor;
        ScreenBox target;
        if (monitorOnCanvas(ScreenBox { m_monitorRect.left, m_monitorRect.top, m_monitorRect.right,
                                m_monitorRect.bottom },
                ScreenBox { game.left, game.top, game.right, game.bottom }, target)) {
            bounds = Rect { static_cast<float>(target.left), static_cast<float>(target.top),
                static_cast<float>(target.right), static_cast<float>(target.bottom) };
            if (painted.anchorMode == OverlayAnchorMode::Free) {
                if (!painted.savedMonitorId.empty()) {
                    painted.originX = painted.monitorOriginX;
                    painted.originY = painted.monitorOriginY;
                } else {
                    const POINT saved { m_monitorRect.left + painted.originX,
                        m_monitorRect.top + painted.originY };
                    MONITORINFO monitor { sizeof(monitor) };
                    const HMONITOR original = MonitorFromPoint(saved, MONITOR_DEFAULTTONULL);
                    if (original != nullptr && GetMonitorInfoW(original, &monitor)) {
                        painted.originX
                            = originOnMonitor(painted.originX, monitor.rcMonitor.left - m_monitorRect.left);
                        painted.originY
                            = originOnMonitor(painted.originY, monitor.rcMonitor.top - m_monitorRect.top);
                    }
                }
            }
        }
    }
    m_contentRectD2D = D2D1::RectF(0.0f, 0.0f, 0.0f, 0.0f);

    m_target->BeginDraw();
    m_target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    const Rect content = paintOverlay(m_target, painted, m_theme, text, bounds, scale, formats,
        &m_rtssLayerBounds, &m_rtssLayerScale, &m_rowBounds);
    m_contentRectD2D = D2D1::RectF(content.left, content.top, content.right, content.bottom);
    m_target->EndDraw();
}

bool OverlayWindow::render()
{
    if (m_window == nullptr) {
        return false;
    }

    // Разрешение или монитор могли смениться, пока оверлей висел поверх игры.
    const RECT currentMonitor = screenRect();
    if (currentMonitor.left != m_monitorRect.left || currentMonitor.top != m_monitorRect.top
        || currentMonitor.right != m_monitorRect.right || currentMonitor.bottom != m_monitorRect.bottom) {
        m_monitorRect = currentMonitor;
        SetWindowPos(m_window, HWND_TOPMOST, m_monitorRect.left, m_monitorRect.top,
            m_monitorRect.right - m_monitorRect.left, m_monitorRect.bottom - m_monitorRect.top,
            SWP_NOACTIVATE);
    }
    const UINT width = static_cast<UINT>(m_monitorRect.right - m_monitorRect.left);
    const UINT height = static_cast<UINT>(m_monitorRect.bottom - m_monitorRect.top);
    // ensureRenderTarget сам решает, нужно ли пересоздавать битмап: при смене разрешения
    // иначе остался бы старый размер и картинка поплыла бы.
    if (!ensureRenderTarget(width, height)) {
        return false;
    }

    OverlaySettings animated = animatedSettings();
    // Прозрачность при появлении и смене стиля: всё содержимое проявляется целиком.
    animated.opacityPercent = static_cast<int>(std::lround(
        static_cast<double>(m_menuSceneOpacity.value_or(animated.opacityPercent)) * m_fade));
    const OverlaySettings saved = m_settings;
    m_settings = animated;
    drawScene(static_cast<float>(std::clamp(animated.scalePercent, 70, 160)) / 100.0f);
    m_settings = saved;

    HDC screen = GetDC(nullptr);
    POINT destination{ m_monitorRect.left, m_monitorRect.top };
    POINT source{ 0, 0 };
    SIZE size{ static_cast<LONG>(m_bitmapWidth), static_cast<LONG>(m_bitmapHeight) };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const BOOL updated = UpdateLayeredWindow(m_window, screen, &destination, &size, m_memoryDc, &source, 0, &blend,
        ULW_ALPHA);
    ReleaseDC(nullptr, screen);
    return updated != FALSE;
}

bool OverlayWindow::capturePng(const std::wstring& path, std::string& error)
{
    if (m_window == nullptr) {
        error = "окно оверлея не создано";
        return false;
    }

    const UINT screenWidth = static_cast<UINT>(m_monitorRect.right - m_monitorRect.left);
    const UINT screenHeight = static_cast<UINT>(m_monitorRect.bottom - m_monitorRect.top);
    if (m_target == nullptr && !ensureRenderTarget(screenWidth, screenHeight)) {
        error = "не удалось подготовить поверхность оверлея";
        return false;
    }

    drawScene(static_cast<float>(std::clamp(m_settings.scalePercent, 70, 160)) / 100.0f);

    if (m_pixels == nullptr || m_contentRectD2D.right <= m_contentRectD2D.left) {
        error = "оверлей ничего не нарисовал";
        return false;
    }

    // Обрезаем прозрачные поля вокруг содержимого: в PNG попадает только сам оверлей.
    const LONG left = std::max(0L, static_cast<LONG>(m_contentRectD2D.left) - 2L);
    const LONG top = std::max(0L, static_cast<LONG>(m_contentRectD2D.top) - 2L);
    const LONG right = std::min(static_cast<LONG>(m_bitmapWidth),
        static_cast<LONG>(m_contentRectD2D.right) + 2L);
    const LONG bottom = std::min(static_cast<LONG>(m_bitmapHeight),
        static_cast<LONG>(m_contentRectD2D.bottom) + 2L);

    const UINT width = static_cast<UINT>(right - left);
    const UINT height = static_cast<UINT>(bottom - top);
    const auto* source = static_cast<const std::uint8_t*>(m_pixels);
    const std::size_t stride = static_cast<std::size_t>(m_bitmapWidth) * 4U;

    std::vector<std::uint8_t> cropped(static_cast<std::size_t>(width) * height * 4U);
    for (UINT row = 0; row < height; ++row) {
        const std::uint8_t* from = source + (static_cast<std::size_t>(top + static_cast<LONG>(row)) * stride)
            + static_cast<std::size_t>(left) * 4U;
        std::memcpy(cropped.data() + static_cast<std::size_t>(row) * width * 4U, from,
            static_cast<std::size_t>(width) * 4U);
    }

    IWICImagingFactory* factory = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory)))) {
        error = "WIC недоступен";
        return false;
    }

    IWICStream* stream = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IWICBitmap* bitmap = nullptr;
    bool ok = false;

    do {
        if (FAILED(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppPBGRA, width * 4U,
                static_cast<UINT>(cropped.size()), cropped.data(), &bitmap))) {
            error = "WIC не принял пиксели оверлея";
            break;
        }
        if (FAILED(factory->CreateStream(&stream))
            || FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
            error = "не удалось открыть " + narrow(path);
            break;
        }
        if (FAILED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder))
            || FAILED(encoder->Initialize(stream, WICBitmapEncoderNoCache))) {
            error = "не удалось создать PNG-кодировщик";
            break;
        }
        if (FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr))) {
            error = "не удалось создать кадр PNG";
            break;
        }
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppPBGRA;
        if (FAILED(frame->SetSize(width, height)) || FAILED(frame->SetPixelFormat(&format))
            || FAILED(frame->WriteSource(bitmap, nullptr)) || FAILED(frame->Commit())
            || FAILED(encoder->Commit())) {
            error = "не удалось записать PNG";
            break;
        }
        ok = true;
    } while (false);

    if (bitmap != nullptr) {
        bitmap->Release();
    }
    if (frame != nullptr) {
        frame->Release();
    }
    if (encoder != nullptr) {
        encoder->Release();
    }
    if (stream != nullptr) {
        stream->Release();
    }
    factory->Release();
    return ok;
}

} // namespace yawa
