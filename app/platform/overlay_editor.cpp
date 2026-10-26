#include "platform/overlay_editor.h"

#include "metrics/overlay_metric.h"
#include "metrics/overlay_rows.h"
#include "platform/win_util.h"
#include "render/overlay_painter.h"
#include "ui/layout.h"
#include "ui/overlay_view.h"

#include <windowsx.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <utility>

namespace yawa {
namespace {
    constexpr wchar_t kEditorClass[] = L"YawaFpsHubOverlayEditor";
    constexpr int kCanvasWidth = 520;
    constexpr int kMaxY = 1490;
    constexpr int kControlIdLabel = 2001;
    constexpr int kControlIdLabelX = 2002;
    constexpr int kControlIdValueX = 2003;
    constexpr int kControlIdY = 2004;
    constexpr std::array<std::wstring_view, 5> kSections { L"Стили", L"Вид", L"Текст", L"Подписи",
        L"Строки" };
    constexpr std::array<int, 5> kSectionOffsets { 0, 282, 810, 1746, 1992 };

    void input(HWND window, const wchar_t* value)
    {
        if (window) {
            SetWindowTextW(window, value);
        }
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

bool OverlayEditor::open(HWND owner, HINSTANCE instance, AppModel& model, const Theme& theme,
    std::function<void(const UiHit&, float, bool)> onAction, std::function<void(wchar_t)> onText,
    std::function<void(bool)> onChange)
{
    if (m_window) {
        ShowWindow(m_window, SW_RESTORE);
        SetForegroundWindow(m_window);
        return true;
    }
    std::string error;
    if (!m_renderer.initialize(error)) {
        return false;
    }
    WNDCLASSW cls { };
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.lpszClassName = kEditorClass;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        m_renderer.shutdown();
        return false;
    }
    m_model = &model;
    m_theme = &theme;
    m_onAction = std::move(onAction);
    m_onText = std::move(onText);
    m_onChange = std::move(onChange);
    m_window = CreateWindowExW(WS_EX_APPWINDOW, kEditorClass, L"Мастерская оверлея · YawaFpsHub",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1320, 920, owner, nullptr, instance, this);
    if (!m_window) {
        m_renderer.shutdown();
        m_model = nullptr;
        return false;
    }
    const auto createEdit = [this, instance](int id, DWORD style) {
        return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0,
            0, 10, 24, m_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    };
    m_labelEdit = createEdit(kControlIdLabel, ES_AUTOHSCROLL);
    m_labelX = createEdit(kControlIdLabelX, ES_NUMBER | ES_AUTOHSCROLL);
    m_valueX = createEdit(kControlIdValueX, ES_NUMBER | ES_AUTOHSCROLL);
    m_y = createEdit(kControlIdY, ES_NUMBER | ES_AUTOHSCROLL);
    m_catalogSearch = createEdit(2005, ES_AUTOHSCROLL);
    if (!m_labelEdit || !m_labelX || !m_valueX || !m_y || !m_catalogSearch) {
        close();
        return false;
    }
    SendMessageW(m_labelEdit, EM_SETLIMITTEXT, 64, 0);
    SendMessageW(m_catalogSearch, EM_SETLIMITTEXT, 64, 0);
    ShowWindow(m_catalogSearch, SW_HIDE);
    layout();
    const auto rows = overlayMetricRows(model.settings.overlay, model.store);
    if (!rows.empty()) {
        select(rows.front().id, false);
    }
    ShowWindow(m_window, SW_SHOW);
    m_onChange(true);
    return true;
}

LRESULT CALLBACK OverlayEditor::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        return TRUE;
    }
    auto* editor = reinterpret_cast<OverlayEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    return editor ? editor->handle(message, wParam, lParam) : DefWindowProcW(window, message, wParam, lParam);
}

void OverlayEditor::layout()
{
    RECT client { };
    GetClientRect(m_window, &client);
    const float height = static_cast<float>(client.bottom);
    m_canvas = Rect { 20.0f, 307.0f, 20.0f + kCanvasWidth, height - 213.0f };
    m_canvasScroll
        = std::clamp(m_canvasScroll, 0, std::max(0, kMaxY + 26 - static_cast<int>(m_canvas.height())));
    m_inspector = Rect { 20.0f, height - 198.0f, 540.0f, height - 20.0f };
    m_controls = Rect { 564.0f, 112.0f, static_cast<float>(client.right - 18), height - 20.0f };
    m_catalog = Rect { static_cast<float>(client.right - 386), 50.0f, static_cast<float>(client.right - 18),
        std::min(height - 22.0f, 462.0f) };
    m_panelScroll = std::clamp(m_panelScroll, 0,
        std::max(0,
            static_cast<int>(overlayEditorControlsHeight(m_model->settings.overlay) - m_controls.height())));
    if (!m_labelEdit) {
        return;
    }
    const int y = static_cast<int>(m_inspector.top);
    SetWindowPos(m_labelEdit, nullptr, 112, y + 44, 390, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_labelX, nullptr, 106, y + 92, 58, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_valueX, nullptr, 275, y + 92, 58, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_y, nullptr, 408, y + 92, 58, 26, SWP_NOZORDER | SWP_NOACTIVATE);
    SetWindowPos(m_catalogSearch, nullptr, client.right - 380, 16, 256, 26, SWP_NOZORDER | SWP_NOACTIVATE);
}

void OverlayEditor::syncFields()
{
    if (!m_model || !m_labelEdit) {
        return;
    }
    const OverlaySettings& settings = m_model->settings.overlay;
    const auto row = canvasPosition(m_selected);
    std::string label(metricInfo(m_selected).label);
    for (const auto& entry : overlayMetricRows(settings, m_model->store)) {
        if (entry.id == m_selected) {
            label = entry.label;
            break;
        }
    }
    m_syncing = true;
    const std::wstring shown = widen(label);
    input(m_labelEdit, shown.c_str());
    input(m_labelX, std::to_wstring(row.labelX).c_str());
    input(m_valueX, std::to_wstring(row.valueX).c_str());
    input(m_y, std::to_wstring(row.y).c_str());
    m_syncing = false;
    const bool isLine = settings.layout == OverlayLayout::Line;
    EnableWindow(m_labelX, !isLine);
    EnableWindow(m_valueX, !isLine);
    EnableWindow(m_y, !isLine);
}

void OverlayEditor::select(MetricId id, bool value)
{
    m_selected = id;
    m_value = value;
    m_model->editingLabel = -1;
    syncFields();
    refresh();
}

void OverlayEditor::changed(bool finished)
{
    if (m_onChange) {
        m_onChange(finished);
    }
    refresh();
}

OverlayRowPosition OverlayEditor::canvasPosition(MetricId id) const
{
    const auto& settings = m_model->settings.overlay;
    const auto position = settings.rowPositions[static_cast<std::size_t>(id)];
    if (settings.pixelLayout && position.y >= 0) {
        return position;
    }
    int index = 0;
    for (const auto& row : overlayMetricRows(settings, m_model->store)) {
        if (row.id == id) {
            break;
        }
        ++index;
    }
    return OverlayRowPosition { 12, 252, std::min(12 + index * 28, kMaxY) };
}

void OverlayEditor::preparePixelLayout()
{
    auto& settings = m_model->settings.overlay;
    if (settings.pixelLayout) {
        return;
    }
    const auto rows = overlayMetricRows(settings, m_model->store);
    for (const auto& row : rows) {
        settings.rowPositions[static_cast<std::size_t>(row.id)] = canvasPosition(row.id);
    }
    settings.pixelLayout = true;
}

void OverlayEditor::move(int x, int y)
{
    if (!m_dragging) {
        return;
    }
    auto& settings = m_model->settings.overlay;
    if (m_dragLine) {
        const Rect inner = m_canvas.inset(kCardPadding);
        const Rect scene { inner.left, inner.top + 46.0f, inner.right, inner.bottom };
        settings.offsetX = std::clamp(m_offsetStartX
                + scaledOverlayOffset(x - m_dragStartX, scene.width(), GetSystemMetrics(SM_CXVIRTUALSCREEN)),
            -8192, 8192);
        settings.offsetY = std::clamp(m_offsetStartY
                + scaledOverlayOffset(y - m_dragStartY, scene.height(), GetSystemMetrics(SM_CYVIRTUALSCREEN)),
            -8192, 8192);
    } else {
        const auto& start = m_dragPositions.front();
        const int originalX = m_value ? start.valueX : start.labelX;
        const int nextX = std::clamp(x - static_cast<int>(m_canvas.left) - m_grabX, 0, 470);
        const int nextY = std::clamp(y - static_cast<int>(m_canvas.top) + m_canvasScroll - m_grabY, 0, kMaxY);
        int deltaX = nextX - originalX;
        int deltaY = nextY - start.y;
        for (const auto& position : m_dragPositions) {
            deltaX = std::clamp(deltaX, -std::min(position.labelX, position.valueX),
                470 - std::max(position.labelX, position.valueX));
            deltaY = std::clamp(deltaY, -position.y, kMaxY - position.y);
        }
        for (std::size_t index = 0; index < m_selectedRows.size(); ++index) {
            auto& row = settings.rowPositions[static_cast<std::size_t>(m_selectedRows[index])];
            row.labelX = m_dragPositions[index].labelX + deltaX;
            row.valueX = m_dragPositions[index].valueX + deltaX;
            row.y = m_dragPositions[index].y + deltaY;
        }
        syncFields();
    }
    changed(false);
}

void OverlayEditor::editCoordinate(HWND field)
{
    if (m_syncing || !field || m_model->settings.overlay.layout == OverlayLayout::Line) {
        return;
    }
    wchar_t buffer[24] { };
    GetWindowTextW(field, buffer, 24);
    wchar_t* end = nullptr;
    const long number = std::wcstol(buffer, &end, 10);
    if (end != buffer && *end == L'\0') {
        preparePixelLayout();
        auto& row = m_model->settings.overlay.rowPositions[static_cast<std::size_t>(m_selected)];
        int& selected = field == m_labelX ? row.labelX : field == m_valueX ? row.valueX : row.y;
        selected = std::clamp(static_cast<int>(number), 0, field == m_y ? kMaxY : 470);
        m_model->settings.overlay.pixelLayout = true;
        changed(true);
    }
    syncFields();
}

std::vector<MetricId> OverlayEditor::catalogRows() const
{
    wchar_t query[65] { };
    if (m_catalogSearch) {
        GetWindowTextW(m_catalogSearch, query, 65);
    }
    std::wstring filter(query);
    const auto fold = [](std::wstring& value) {
        for (wchar_t& symbol : value) {
            symbol = symbol >= L'А' && symbol <= L'Я' ? static_cast<wchar_t>(symbol + 32)
                : symbol == L'Ё'                      ? L'ё'
                                                      : static_cast<wchar_t>(std::towlower(symbol));
        }
    };
    fold(filter);
    std::vector<MetricId> result;
    for (const MetricInfo& info : kMetricTable) {
        if (displayMetric(info.id) != info.id) {
            continue;
        }
        std::wstring label = widen(info.label);
        std::wstring key = widen(info.key);
        fold(label);
        fold(key);
        if (filter.empty() || label.find(filter) != std::wstring::npos
            || key.find(filter) != std::wstring::npos) {
            result.push_back(info.id);
        }
    }
    return result;
}

const UiHit* OverlayEditor::hitAt(float x, float y) const
{
    if (!m_controls.contains(x, y)) {
        return nullptr;
    }
    for (auto it = m_hits.rbegin(); it != m_hits.rend(); ++it) {
        if (it->rect.contains(x, y)) {
            return &*it;
        }
    }
    return nullptr;
}

LRESULT OverlayEditor::handle(UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CLOSE:
        close();
        return 0;
    case WM_NCDESTROY: {
        const HWND window = m_window;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        m_window = nullptr;
        m_labelEdit = m_labelX = m_valueX = m_y = m_catalogSearch = nullptr;
        m_renderer.shutdown();
        m_model = nullptr;
        m_hits.clear();
        m_onAction = { };
        m_onText = { };
        m_onChange = { };
        return DefWindowProcW(window, message, wParam, lParam);
    }
    case WM_SIZE:
        m_renderer.resize(LOWORD(lParam), HIWORD(lParam), 1.0f);
        if (m_model) {
            layout();
        }
        refresh();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
        info->ptMinTrackSize = POINT { 1000, 720 };
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps { };
        BeginPaint(m_window, &ps);
        paint();
        EndPaint(m_window, &ps);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        POINT cursor { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        ScreenToClient(m_window, &cursor);
        const int delta = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA * 65;
        if (m_catalogOpen && m_catalog.contains(static_cast<float>(cursor.x), static_cast<float>(cursor.y))) {
            m_catalogScroll = std::clamp(
                m_catalogScroll - delta / 28, 0, std::max(0, static_cast<int>(catalogRows().size()) - 12));
        } else if (cursor.x < static_cast<int>(m_controls.left)) {
            // Последнюю строку (y=1490) тоже можно довести до видимой области.
            const int scrollLimit = std::max(0, kMaxY + 26 - static_cast<int>(m_canvas.height()));
            m_canvasScroll = std::clamp(m_canvasScroll - delta, 0, scrollLimit);
        } else {
            m_panelScroll = std::clamp(m_panelScroll - delta, 0,
                std::max(0,
                    static_cast<int>(
                        overlayEditorControlsHeight(m_model->settings.overlay) - m_controls.height())));
        }
        refresh();
        return 0;
    }
    case WM_COMMAND: {
        const HWND field = reinterpret_cast<HWND>(lParam);
        if (field == m_catalogSearch && HIWORD(wParam) == EN_CHANGE) {
            m_catalogScroll = 0;
            refresh();
        } else if (field == m_labelEdit && HIWORD(wParam) == EN_CHANGE && !m_syncing) {
            wchar_t label[65] { };
            GetWindowTextW(field, label, 65);
            setOverlayMetricLabel(m_model->settings.overlay, m_selected, narrow(label));
            changed(false);
        } else if (field == m_labelEdit && HIWORD(wParam) == EN_KILLFOCUS) {
            changed(true);
        } else if ((field == m_labelX || field == m_valueX || field == m_y)
            && HIWORD(wParam) == EN_KILLFOCUS) {
            editCoordinate(field);
        }
        return 0;
    }
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        if (x >= static_cast<int>(m_controls.right - 108.0f) && y >= 12 && y < 46) {
            m_catalogOpen = !m_catalogOpen;
            ShowWindow(m_catalogSearch, m_catalogOpen ? SW_SHOW : SW_HIDE);
            if (m_catalogOpen) {
                SetFocus(m_catalogSearch);
            } else {
                SetFocus(m_window);
            }
            refresh();
            return 0;
        }
        if (m_catalogOpen && m_catalog.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int item = (y - static_cast<int>(m_catalog.top) - 34) / 28;
            const auto options = catalogRows();
            const int index = item + m_catalogScroll;
            if (y >= m_catalog.top + 34 && item >= 0 && item < 12 && index >= 0
                && index < static_cast<int>(options.size())) {
                auto& settings = m_model->settings.overlay;
                const MetricId id = options[static_cast<std::size_t>(index)];
                const std::size_t metric = static_cast<std::size_t>(id);
                settings.metrics[metric] = !settings.metrics[metric];
                for (MetricId detail : kPairedMetricDetails) {
                    if (displayMetric(detail) == id) {
                        settings.metrics[static_cast<std::size_t>(detail)] = false;
                    }
                }
                if (settings.metrics[metric]) {
                    if (settings.pixelLayout) {
                        enableOverlayPixelLayout(settings);
                    }
                    select(id, false);
                }
                changed(true);
            }
            return 0;
        }
        if (m_catalogOpen) {
            m_catalogOpen = false;
            ShowWindow(m_catalogSearch, SW_HIDE);
        }
        if (x >= static_cast<int>(m_controls.left) && y >= 67 && y < 102) {
            const float width = m_controls.width() / static_cast<float>(kSections.size());
            const int tab = static_cast<int>((x - m_controls.left) / width);
            if (tab >= 0 && tab < static_cast<int>(kSections.size())) {
                m_panelScroll = std::min(kSectionOffsets[static_cast<std::size_t>(tab)],
                    std::max(0,
                        static_cast<int>(
                            overlayEditorControlsHeight(m_model->settings.overlay) - m_controls.height())));
                refresh();
            }
            return 0;
        }
        if (x >= 405 && x < 540 && y >= 283 && y < 307) {
            m_model->settings.overlay.pixelLayout = false;
            changed(true);
            return 0;
        }
        if (m_inspector.contains(static_cast<float>(x), static_cast<float>(y))) {
            const int localX = x - static_cast<int>(m_inspector.left);
            const int localY = y - static_cast<int>(m_inspector.top);
            if (localY < 35 && localX > 278) {
                if (localX > 405) {
                    auto& settings = m_model->settings.overlay;
                    settings.metrics[static_cast<std::size_t>(m_selected)] = false;
                    for (MetricId detail : kPairedMetricDetails) {
                        if (displayMetric(detail) == m_selected) {
                            settings.metrics[static_cast<std::size_t>(detail)] = false;
                        }
                    }
                    changed(true);
                    const auto rows = overlayMetricRows(settings, m_model->store);
                    if (!rows.empty()) {
                        select(rows.front().id, false);
                    }
                } else {
                    m_model->settings.overlay.customLabelSet[static_cast<std::size_t>(m_selected)] = false;
                    syncFields();
                    changed(true);
                }
                return 0;
            }
            if (localY >= 137 && localY <= 166) {
                const int color = (localX - 20) / 56 - 1;
                if (localX >= 20 && color >= -1 && color < kOverlayColorCount) {
                    m_model->settings.overlay.metricColors[static_cast<std::size_t>(m_selected)] = color;
                    changed(true);
                }
                return 0;
            }
        }
        if (m_canvas.contains(static_cast<float>(x), static_cast<float>(y))) {
            if (m_model->settings.overlay.layout == OverlayLayout::Line) {
                m_dragLine = true;
                m_dragging = true;
                m_dragStartX = x;
                m_dragStartY = y;
                m_offsetStartX = m_model->settings.overlay.offsetX;
                m_offsetStartY = m_model->settings.overlay.offsetY;
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
            for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
                const auto row = canvasPosition(it->id);
                const int rowY = static_cast<int>(m_canvas.top) + row.y - m_canvasScroll;
                if (y < rowY || y >= rowY + 26) {
                    continue;
                }
                const bool value = x >= m_canvas.left + row.valueX;
                if (!value && (x < m_canvas.left + row.labelX || x >= m_canvas.left + row.labelX + 230)) {
                    continue;
                }
                const bool isSelected
                    = std::find(m_selectedRows.begin(), m_selectedRows.end(), it->id) != m_selectedRows.end();
                if (GetKeyState(VK_SHIFT) < 0) {
                    if (isSelected) {
                        m_selectedRows.erase(
                            std::remove(m_selectedRows.begin(), m_selectedRows.end(), it->id),
                            m_selectedRows.end());
                    } else {
                        m_selectedRows.push_back(it->id);
                    }
                    select(it->id, value);
                    return 0;
                }
                if (!isSelected) {
                    m_selectedRows = { it->id };
                }
                select(it->id, value);
                preparePixelLayout();
                m_dragPositions.clear();
                m_dragPositions.push_back(
                    m_model->settings.overlay.rowPositions[static_cast<std::size_t>(it->id)]);
                std::vector<MetricId> ordered { it->id };
                for (MetricId selected : m_selectedRows) {
                    if (selected != it->id) {
                        ordered.push_back(selected);
                        m_dragPositions.push_back(
                            m_model->settings.overlay.rowPositions[static_cast<std::size_t>(selected)]);
                    }
                }
                m_selectedRows = std::move(ordered);
                m_dragLine = false;
                m_grabX = x - static_cast<int>(m_canvas.left) - (value ? row.valueX : row.labelX);
                m_grabY = y - rowY;
                m_dragging = true;
                SetFocus(m_window);
                SetCapture(m_window);
                return 0;
            }
            return 0;
        }
        if (const UiHit* hit = hitAt(static_cast<float>(x), static_cast<float>(y))) {
            m_activeHit = *hit;
            SetFocus(m_window);
            if (hit->action == ActionKind::ReorderMetric || isSliderAction(hit->action)) {
                SetCapture(m_window);
                if (isSliderAction(hit->action)) {
                    m_onAction(*hit, static_cast<float>(x), false);
                }
            } else {
                m_onAction(*hit, static_cast<float>(x), true);
            }
            refresh();
        }
        return 0;
    }
    case WM_MOUSEMOVE:
        if (m_dragging) {
            move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        } else if (GetCapture() == m_window && isSliderAction(m_activeHit.action)) {
            m_onAction(m_activeHit, static_cast<float>(GET_X_LPARAM(lParam)), false);
            refresh();
        } else if (GetCapture() == m_window && m_activeHit.action == ActionKind::ReorderMetric) {
            if (const UiHit* hit
                = hitAt(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)))) {
                if (hit->action == ActionKind::ReorderMetric && hit->value != m_activeHit.value) {
                    moveOverlayMetric(m_model->settings.overlay, static_cast<MetricId>(m_activeHit.value),
                        static_cast<MetricId>(hit->value));
                    changed(false);
                }
            }
        }
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == m_window) {
            if (m_dragging) {
                move(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
                m_dragging = false;
                changed(true);
            } else if (isSliderAction(m_activeHit.action)) {
                m_onAction(m_activeHit, static_cast<float>(GET_X_LPARAM(lParam)), true);
            } else if (m_activeHit.action == ActionKind::ReorderMetric) {
                changed(true);
            }
            m_activeHit.action = ActionKind::None;
            ReleaseCapture();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (m_model->editingLabel >= 0) {
                m_onText(27);
            } else {
                close();
            }
            return 0;
        }
        if (m_model->editingLabel >= 0) {
            break;
        }
        if (wParam == VK_TAB) {
            m_value = !m_value;
            refresh();
            return 0;
        }
        if (wParam == VK_LEFT || wParam == VK_RIGHT || wParam == VK_UP || wParam == VK_DOWN) {
            if (m_model->settings.overlay.layout == OverlayLayout::Line) {
                auto& settings = m_model->settings.overlay;
                const int delta = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
                if (wParam == VK_LEFT || wParam == VK_RIGHT) {
                    settings.offsetX
                        = std::clamp(settings.offsetX + (wParam == VK_LEFT ? -delta : delta), -8192, 8192);
                } else {
                    settings.offsetY
                        = std::clamp(settings.offsetY + (wParam == VK_UP ? -delta : delta), -8192, 8192);
                }
                changed(true);
                return 0;
            }
            preparePixelLayout();
            auto& settings = m_model->settings.overlay;
            const int delta = GetKeyState(VK_SHIFT) < 0 ? 10 : 1;
            if (m_selectedRows.empty()) {
                m_selectedRows.push_back(m_selected);
            }
            int deltaX = wParam == VK_LEFT ? -delta : wParam == VK_RIGHT ? delta : 0;
            int deltaY = wParam == VK_UP ? -delta : wParam == VK_DOWN ? delta : 0;
            for (MetricId id : m_selectedRows) {
                const auto& row = settings.rowPositions[static_cast<std::size_t>(id)];
                deltaX = std::clamp(
                    deltaX, -std::min(row.labelX, row.valueX), 470 - std::max(row.labelX, row.valueX));
                deltaY = std::clamp(deltaY, -row.y, kMaxY - row.y);
            }
            for (MetricId id : m_selectedRows) {
                auto& row = settings.rowPositions[static_cast<std::size_t>(id)];
                row.labelX += deltaX;
                row.valueX += deltaX;
                row.y += deltaY;
            }
            settings.pixelLayout = true;
            syncFields();
            changed(true);
            return 0;
        }
        break;
    case WM_CHAR:
        if (m_model->editingLabel >= 0) {
            m_onText(static_cast<wchar_t>(wParam));
            refresh();
            return 0;
        }
        break;
    }
    return DefWindowProcW(m_window, message, wParam, lParam);
}

void OverlayEditor::paint()
{
    if (!m_renderer.begin(m_window)) {
        return;
    }
    m_renderer.target()->SetDpi(96.0f, 96.0f); // координаты холста соответствуют физическим пикселям
    const Theme& theme = *m_theme;
    m_renderer.clear(theme.background);
    const float width = m_renderer.width();
    const float height = m_renderer.height();
    m_renderer.fillRadialGlow(250.0f, -80.0f, 490.0f, theme.accent.withAlpha(0.12f));
    m_renderer.text(
        L"Мастерская оверлея", Rect { 20.0f, 12.0f, 510.0f, 44.0f }, TextRole::Heading, theme.text);
    m_renderer.text(L"Слева — живой вид и расстановка. Справа — ВСЕ настройки.",
        Rect { 20.0f, 37.0f, 540.0f, 61.0f }, TextRole::Small, theme.textDim);

    drawOverlayEditorPreview(m_renderer, *m_model, theme, Rect { 20.0f, 70.0f, 540.0f, 282.0f },
        GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN));
    m_renderer.text(m_model->settings.overlay.layout == OverlayLayout::Line
            ? L"Строка: двигайте весь блок; порядок — в разделе «Строки»"
            : L"Холст: Shift + клик выделяет несколько блоков",
        Rect { 20.0f, 284.0f, 395.0f, 304.0f }, TextRole::Tiny, theme.textDim);
    m_renderer.text(L"Вернуть шаблон", Rect { 405.0f, 283.0f, 540.0f, 307.0f }, TextRole::Tiny, theme.accent);
    const auto rows = overlayMetricRows(m_model->settings.overlay, m_model->store);
    m_renderer.fillRoundRect(m_canvas, 10.0f, theme.panel);
    m_renderer.pushClip(m_canvas);
    if (m_model->settings.overlay.layout == OverlayLayout::Line) {
        drawOverlayEditorPreview(m_renderer, *m_model, theme, m_canvas, GetSystemMetrics(SM_CXVIRTUALSCREEN),
            GetSystemMetrics(SM_CYVIRTUALSCREEN));
    } else {
        for (int y = -m_canvasScroll % 28; y < static_cast<int>(m_canvas.height()); y += 28) {
            m_renderer.drawLine(m_canvas.left, m_canvas.top + static_cast<float>(y), m_canvas.right,
                m_canvas.top + static_cast<float>(y), theme.line.withAlpha(0.45f), 1.0f);
        }
        for (const auto& entry : rows) {
            const std::size_t index = static_cast<std::size_t>(entry.id);
            const auto row = canvasPosition(entry.id);
            if (row.y < m_canvasScroll - 28 || row.y > m_canvasScroll + m_canvas.height()) {
                continue;
            }
            const float y = m_canvas.top + static_cast<float>(row.y - m_canvasScroll);
            const bool active = m_selected == entry.id
                || std::find(m_selectedRows.begin(), m_selectedRows.end(), entry.id) != m_selectedRows.end();
            if (active) {
                const float left = m_canvas.left + static_cast<float>(std::min(row.labelX, row.valueX));
                const float right
                    = m_canvas.left + static_cast<float>(std::max(row.labelX + 230, row.valueX + 220));
                m_renderer.fillRoundRect(
                    Rect { left - 2.0f, y - 2.0f, right, y + 27.0f }, 5.0f, theme.accentSoft);
            }
            const auto palette = overlayColorPalette();
            const int color = m_model->settings.overlay.metricColors[index];
            const int labelEnd = row.labelX == row.valueX ? row.labelX
                : row.labelX < row.valueX                 ? std::min(row.labelX + 228, row.valueX - 8)
                                                          : 512;
            const int valueEnd = row.valueX < row.labelX ? row.labelX - 8 : 512;
            if (labelEnd > row.labelX) {
                m_renderer.text(widen(entry.label),
                    Rect { m_canvas.left + row.labelX, y, m_canvas.left + labelEnd, y + 26.0f },
                    TextRole::Small, active && !m_value ? theme.accent : theme.textDim);
            }
            if (valueEnd > row.valueX) {
                m_renderer.text(widen(entry.value),
                    Rect { m_canvas.left + row.valueX, y, m_canvas.left + valueEnd, y + 26.0f },
                    TextRole::Small, color >= 0 ? palette[static_cast<std::size_t>(color)] : theme.text);
            }
        }
    }
    m_renderer.popClip();
    m_renderer.strokeRoundRect(m_canvas, 10.0f, theme.line);

    m_renderer.fillRoundRect(m_inspector, 10.0f, theme.panel);
    const std::wstring title
        = rows.empty() ? L"Добавьте показатель через «+ Датчик»" : widen(metricInfo(m_selected).label);
    m_renderer.text(clipText(m_renderer, title, TextRole::Heading, 280.0f),
        Rect { 32.0f, m_inspector.top + 7.0f, 320.0f, m_inspector.top + 34.0f }, TextRole::Heading,
        theme.text);
    m_renderer.text(L"Сбросить имя", Rect { 320.0f, m_inspector.top + 9.0f, 425.0f, m_inspector.top + 32.0f },
        TextRole::Tiny, theme.accent);
    m_renderer.text(L"Скрыть", Rect { 434.0f, m_inspector.top + 9.0f, 508.0f, m_inspector.top + 32.0f },
        TextRole::Tiny, theme.warn);
    m_renderer.text(L"Подпись", Rect { 32.0f, m_inspector.top + 46.0f, 103.0f, m_inspector.top + 67.0f },
        TextRole::Small, theme.textDim);
    m_renderer.text(L"X подписи", Rect { 32.0f, m_inspector.top + 96.0f, 106.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    m_renderer.text(L"X значения", Rect { 182.0f, m_inspector.top + 96.0f, 276.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    m_renderer.text(L"Y", Rect { 380.0f, m_inspector.top + 96.0f, 403.0f, m_inspector.top + 119.0f },
        TextRole::Tiny, theme.textDim);
    const auto palette = overlayColorPalette();
    const int selectedColor = m_model->settings.overlay.metricColors[static_cast<std::size_t>(m_selected)];
    for (int n = -1; n < kOverlayColorCount; ++n) {
        const float x = 40.0f + static_cast<float>(n + 1) * 56.0f;
        const Rect chip { x, m_inspector.top + 137.0f, x + 30.0f, m_inspector.top + 167.0f };
        m_renderer.fillRoundRect(chip, 8.0f, n < 0 ? theme.panelSoft : palette[static_cast<std::size_t>(n)]);
        if (n == selectedColor) {
            m_renderer.strokeRoundRect(chip, 8.0f, theme.accent, 2.0f);
        }
        if (n < 0) {
            m_renderer.text(L"А", chip, TextRole::Small, theme.text, TextAlign::Center);
        }
    }
    m_renderer.text(L"цвет строки",
        Rect { 435.0f, m_inspector.top + 140.0f, 520.0f, m_inspector.top + 162.0f }, TextRole::Tiny,
        theme.textDim);

    if (width >= 1200.0f) {
        m_renderer.text(L"Оформление · шрифт · подписи · датчики",
            Rect { m_controls.left, 12.0f, width - 400.0f, 44.0f }, TextRole::Heading, theme.text);
    }
    m_renderer.fillRoundRect(Rect { width - 110.0f, 12.0f, width - 18.0f, 45.0f }, 8.0f, theme.accentSoft);
    m_renderer.text(m_catalogOpen ? L"Закрыть" : L"+ Датчик",
        Rect { width - 108.0f, 12.0f, width - 18.0f, 45.0f }, TextRole::Small, theme.accent,
        TextAlign::Center);
    for (std::size_t index = 0; index < kSections.size(); ++index) {
        const float tabWidth = m_controls.width() / static_cast<float>(kSections.size());
        const Rect tab { m_controls.left + tabWidth * static_cast<float>(index), 68.0f,
            m_controls.left + tabWidth * static_cast<float>(index + 1) - 6.0f, 101.0f };
        m_renderer.fillRoundRect(tab, 8.0f, theme.panelSoft);
        m_renderer.text(kSections[index], tab, TextRole::Small, theme.text, TextAlign::Center);
    }
    m_renderer.pushClip(m_controls);
    m_renderer.pushTranslate(0.0f, -static_cast<float>(m_panelScroll));
    m_hits.clear();
    drawOverlayEditorControls(m_renderer, *m_model, theme,
        Rect { m_controls.left, m_controls.top + 4.0f, m_controls.right - 6.0f,
            m_controls.top + overlayEditorControlsHeight(m_model->settings.overlay) },
        m_hits);
    m_renderer.popTranslate();
    m_renderer.popClip();
    for (UiHit& hit : m_hits) {
        hit.rect.top -= static_cast<float>(m_panelScroll);
        hit.rect.bottom -= static_cast<float>(m_panelScroll);
    }
    const float fraction
        = m_panelScroll / std::max(overlayEditorControlsHeight(m_model->settings.overlay), 1.0f);
    m_renderer.fillRoundRect(Rect { m_controls.right - 4.0f, m_controls.top + fraction * m_controls.height(),
                                 m_controls.right, m_controls.top + fraction * m_controls.height() + 55.0f },
        2.0f, theme.accent);
    if (m_catalogOpen) {
        m_renderer.fillRoundRect(m_catalog, 10.0f, theme.panel);
        m_renderer.strokeRoundRect(m_catalog, 10.0f, theme.accent, 1.0f);
        m_renderer.text(L"Поиск датчиков · нажмите строку, чтобы включить",
            Rect {
                m_catalog.left + 12.0f, m_catalog.top + 6.0f, m_catalog.right - 8.0f, m_catalog.top + 30.0f },
            TextRole::Tiny, theme.textDim);
        const auto options = catalogRows();
        const int count = std::min(12, static_cast<int>(options.size()) - m_catalogScroll);
        for (int index = 0; index < count; ++index) {
            const MetricId id = options[static_cast<std::size_t>(index + m_catalogScroll)];
            const float y = m_catalog.top + 34.0f + static_cast<float>(index) * 28.0f;
            const bool selected = m_model->settings.overlay.metrics[static_cast<std::size_t>(id)];
            if (selected) {
                m_renderer.fillRoundRect(Rect { m_catalog.left + 6.0f, y, m_catalog.right - 6.0f, y + 26.0f },
                    5.0f, theme.accentSoft);
            }
            m_renderer.text(widen(metricInfo(id).label),
                Rect { m_catalog.left + 12.0f, y, m_catalog.right - 62.0f, y + 26.0f }, TextRole::Small,
                selected ? theme.text : theme.textDim);
            m_renderer.text(selected ? L"вкл" : L"+",
                Rect { m_catalog.right - 57.0f, y, m_catalog.right - 12.0f, y + 26.0f }, TextRole::Tiny,
                selected ? theme.accent : theme.textFaint, TextAlign::Right);
        }
    }
    m_renderer.text(L"←/→/↑/↓: 1 px · Shift: 10 px · Tab: подпись/значение · Esc: закрыть",
        Rect { 20.0f, height - 19.0f, width - 20.0f, height }, TextRole::Tiny, theme.textFaint);
    m_renderer.end();
}

} // namespace yawa
